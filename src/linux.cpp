/* linux.cpp - module containing functions only applicable to Linux and Unix platforms
 *
 * NVGT - NonVisual Gaming Toolkit
 * Copyright (c) 2022-2025 Sam Tupy
 * https://nvgt.dev
 * This software is provided "as-is", without any express or implied warranty. In no event will the authors be held liable for any damages arising from the use of this software.
 * Permission is granted to anyone to use this software for any purpose, including commercial applications, and to alter it and redistribute it freely, subject to the following restrictions:
 * 1. The origin of this software must not be misrepresented; you must not claim that you wrote the original software. If you use this software in a product, an acknowledgment in the product documentation would be appreciated but is not required.
 * 2. Altered source versions must be plainly marked as such, and must not be misrepresented as being the original software.
 * 3. This notice may not be removed or altered from any source distribution.
 */

#if !defined(__ANDROID__) && (defined(__linux__) || defined(__unix__) || defined(__FreeBSD__) || defined(__NetBSD__) || defined(__OpenBSD__) || defined(__DragonFly__))
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <time.h>
#include "linux.h"
#include "tts.h"
#include "tts_prism.h"

using namespace std;

// ============================================================================
// Screen reader layer
// ============================================================================

// Prism's screen reader backends are synchronous: the orca backend in particular blocks in D-Bus calls until the reader answers, and the script thread must never make those calls itself — not even the cheap selection probes, because when the reader's bus name is owned they turn into synchronous calls to the reader, and against a reader that is starting up or inspecting our window (orca does so on every keystroke) they block the script thread until the reader's watchdog kills it. Every prism call below therefore runs on a dedicated worker thread. Three mechanisms keep that cheap and lossless: prism's background availability poll (configured in tts_prism.cpp, callback below) pushes a refresh job to the worker whenever orca appears or vanishes, so the cache tracks reality without anyone probing at call time; output calls are fire and forget, so navigation is never slowed by the reader's pace; and if a delivery fails because the reader died in the gap before the poll noticed, the worker re-speaks that message through speech dispatcher, so nothing is ever lost. An output call made while no reader is installed waits, bounded and via condition variables only, for the reselection it triggers — the one case where waiting is what picks the message up.
static PrismBackendId g_sr_backend_ids[] = {
	PRISM_BACKEND_ORCA,
};

// Used only by the worker's delivery-failure fallback: if the reader died between the cache check and a call, the message is re-spoken through speech dispatcher (the same engine the screen reader itself uses for output) instead of being dropped.
static PrismBackendId g_sr_fallback_ids[] = {
	PRISM_BACKEND_SPEECH_DISPATCHER,
};

struct sr_job {
	enum class kind { refresh, select, speak, output, braille, stop };
	kind action;
	string text;
	bool interrupt = false;
};

// All of the screen reader state is deliberately immortal: it is allocated once and never destroyed, and nothing is ever registered with atexit. prism's screen reader backends block in synchronous D-Bus calls that cannot be cancelled (GIO's default timeout is very long), so joining the worker or tearing the prism context down during exit() can freeze the whole process for seconds with the main thread unresponsive, which is exactly what gets the reader's watchdog killed while it crawls our dying window. A process that is going away does not need to free anything: the kernel reclaims it all and the session bus sees an ordinary disconnect.
struct sr_shared {
	mutex mutex; // One mutex guards the queue, the worker lifecycle and the cached state.
	condition_variable cv;
	deque<sr_job> queue;
	thread *worker = nullptr; // Heap allocated for the same reason as sr() below: a joinable std::thread destroyed by static teardown would call std::terminate.
	PrismBackend *backend = nullptr; // Owned and only mutated by the worker.
	string name;
	uint64_t features = 0;
	chrono::steady_clock::time_point last_select; // Throttles reselection while no reader is installed.
	uint64_t select_requested = 0; // Bumped each time a select job is queued.
	uint64_t select_done = 0; // Bumped by the worker after each select job finishes, successfully or not.
	condition_variable select_cv; // Wakes bounded waiters when a reselection completes.
};

static void sr_release();
static PrismBackend *sr_select();

// Returns the immortal shared state, allocating it on first use. The pointer static is a raw pointer so no destructor ever runs on it: static teardown at exit() must not free anything the worker may still be using.
static sr_shared &sr() {
	static sr_shared *s = new sr_shared;
	return *s;
}

static void sr_worker_loop();

// Pushed to the front of the queue whenever prism's availability poll thread reports that orca appeared or vanished: the worker then re-probes and brings the cache back in line with reality without any script thread ever having to detect the change by failing.
static void sr_refresh() {
	// Worker only: drops a possibly stale cache and publishes orca again if it is there.
	sr_release();
	sr_select();
}

// The availability callback. Runs on prism's poll thread, which must never be blocked, so the only work done here is queueing a refresh for the worker. Transitions for anything but the reader we care about are ignored.
static void sr_availability_changed(void *userdata, PrismBackendId backend, const char *name, bool available) {
	if (backend != PRISM_BACKEND_ORCA) return;
	lock_guard<mutex> lock(sr().mutex);
	sr().queue.push_front(sr_job{sr_job::kind::refresh, string(), false});
	sr().cv.notify_one();
}

// Releases the cached screen reader backend after a failed call.
static void sr_release() {
	PrismBackend *backend;
	{
		lock_guard<mutex> lock(sr().mutex);
		backend = sr().backend;
		sr().backend = nullptr;
		sr().name.clear();
		sr().features = 0;
	}
	if (!backend) return;
	(void)prism_backend_stop(backend);
	prism_backend_free(backend);
}

// Selects the highest priority reader backend that initializes, refreshing the cached state on success. Worker thread only: the registry scan makes synchronous D-Bus calls. Must not be called with sr().mutex held.
static PrismBackend *sr_select() {
	string name;
	uint64_t features = 0;
	PrismBackend *backend = prism_sr_select(prism_get_context(), g_sr_backend_ids, sizeof(g_sr_backend_ids) / sizeof(g_sr_backend_ids[0]), features, name);
	if (backend) {
		lock_guard<mutex> lock(sr().mutex);
		sr().backend = backend;
		sr().features = features;
		sr().name = std::move(name);
	}
	return backend;
}

// Returns the cached backend, selecting one first if needed. Worker thread only.
static PrismBackend *sr_backend_for_job() {
	{
		lock_guard<mutex> lock(sr().mutex);
		if (sr().backend) return sr().backend;
	}
	return sr_select();
}

// Performs one output job on the worker. If the delivery fails because the reader vanished between the cache check and the call, the very message is re-spoken through the speech dispatcher backend, so nothing is ever lost: the script already got true, and this is exactly the fallback it would have chosen, done inside the layer so the script thread never has to wait for delivery results.
static void sr_perform(const sr_job &job) {
	PrismBackend *backend = sr_backend_for_job();
	if (!backend) return;
	PrismError err = PRISM_OK;
	switch (job.action) {
		case sr_job::kind::speak: err = prism_backend_speak(backend, job.text.c_str(), job.interrupt); break;
		case sr_job::kind::output: err = prism_backend_output(backend, job.text.c_str(), job.interrupt); break;
		case sr_job::kind::braille: err = prism_backend_braille(backend, job.text.c_str()); break;
		case sr_job::kind::stop: err = prism_backend_stop(backend); break;
		default: return;
	}
	if (!prism_sr_error_invalidates(err)) return;
	sr_release(); // The backend that just failed is not trustworthy.
	if (job.action != sr_job::kind::speak && job.action != sr_job::kind::output) return;
	uint64_t features = 0;
	string name;
	PrismBackend *fallback = prism_sr_select(prism_get_context(), g_sr_fallback_ids, sizeof(g_sr_fallback_ids) / sizeof(g_sr_fallback_ids[0]), features, name);
	if (!fallback) return; // No voice anywhere either; the message is dropped as a last resort.
	(void)prism_backend_speak(fallback, job.text.c_str(), job.interrupt);
	prism_backend_free(fallback);
}

// The worker: drains the queue forever, keeping every prism call off the script thread. It is never joined: whatever its last call is doing, the process simply outlives it.
static void sr_worker_loop() {
	for (;;) {
		sr_job job;
		{
			unique_lock<mutex> lock(sr().mutex);
			sr().cv.wait(lock, [] { return !sr().queue.empty(); });
			job = std::move(sr().queue.front());
			sr().queue.pop_front();
		}
		try {
			if (job.action == sr_job::kind::refresh) {
				sr_refresh(); // prism's poll thread told us the reader appeared or vanished; sync the cache.
				continue;
			}
			if (job.action == sr_job::kind::select) {
				sr_select(); // Publishes the cache on success; the selection probe is a worker-only call.
				lock_guard<mutex> lock(sr().mutex);
				sr().select_done++;
				sr().select_cv.notify_all(); // Release whoever is bounded-waiting on this reselection.
				continue;
			}
			sr_perform(job);
		} catch (...) {
			sr_release(); // Nothing that failed inside a prism call may escape this thread function: an uncaught exception in a std::thread aborts the whole process.
		}
	}
}

// Starts the worker once. Must be called with sr().mutex held.
static void sr_start_worker_locked() {
	if (sr().worker) return;
	sr().worker = new thread(sr_worker_loop); // Deliberately never joined, see sr().
}

// Schedules a selection on the worker, throttled to one attempt per 250 ms: with no reader installed, scanning the registry on every call would probe its backends nonstop, but a reader that appears must be picked up by the next output call, not several calls later. Must be called with sr().mutex held.
static void sr_request_select_locked() {
	if (chrono::steady_clock::now() - sr().last_select < chrono::milliseconds(250)) return;
	sr().last_select = chrono::steady_clock::now();
	sr_start_worker_locked();
	if (sr().queue.size() >= 32) sr().queue.pop_front(); // If the reader cannot keep up, the oldest pending work is dropped in favor of the newest.
	sr().queue.push_back(sr_job{sr_job::kind::select, string(), false});
	sr().select_requested++;
	sr().cv.notify_one();
}

// Copies the cached feature mask for the script visible queries and, while no reader is selected, schedules the throttled reselection that picks up a reader started later.
static bool sr_state(uint64_t &features) {
	lock_guard<mutex> lock(sr().mutex);
	if (sr().backend) {
		features = sr().features;
		return true;
	}
	sr_request_select_locked();
	return false;
}

// Like sr_state, but when no reader is cached it also waits — bounded, and only on the condition variable, never on prism — for a reselection to finish, so the output call that notices a reader appear is the one that delivers to it instead of falling back while the worker catches up. Output calls bypass the reselection throttle: the fallback that made the script land here just proved the reader is flapping, so the very next output must get a fresh probe even if another one ran less than 250 ms ago; the probe is a worker-only pair of bus name queries, cheap at any keypress rate.
static bool sr_state_wait(uint64_t &features) {
	if (sr_state(features)) return true;
	uint64_t gen;
	{
		lock_guard<mutex> lock(sr().mutex);
		if (sr().backend) {
			features = sr().features;
			return true;
		}
		if (sr().select_requested == sr().select_done) {
			sr().last_select = chrono::steady_clock::now();
			sr_start_worker_locked();
			if (sr().queue.size() >= 32) sr().queue.pop_front();
			sr().queue.push_back(sr_job{sr_job::kind::select, string(), false});
			sr().select_requested++;
			sr().cv.notify_one();
		}
		gen = sr().select_requested;
	}
	unique_lock<mutex> lock(sr().mutex);
	sr().select_cv.wait_for(lock, chrono::milliseconds(500), [&] { return sr().select_done >= gen; });
	if (sr().backend) {
		features = sr().features;
		return true;
	}
	return false;
}

// Must be called with sr().mutex held, after sr_state confirmed a reader with the wanted feature.
static void sr_enqueue_locked(const sr_job &job) {
	if (sr().queue.size() >= 32) sr().queue.pop_front();
	sr().queue.push_back(job);
	sr().cv.notify_one();
}

static void sr_request_select() {
	lock_guard<mutex> lock(sr().mutex);
	sr_request_select_locked();
}

bool screen_reader_load() {
	uint64_t features;
	return sr_state(features);
}
void screen_reader_unload() {
	// Intentionally empty: everything the screen reader layer owns is immortal, see sr().
}
std::string screen_reader_detect() {
	uint64_t features;
	if (!sr_state(features)) return std::string();
	lock_guard<mutex> lock(sr().mutex);
	return sr().name;
}
bool screen_reader_has_speech() {
	uint64_t features;
	return sr_state(features) && (features & PRISM_BACKEND_SUPPORTS_SPEAK) != 0;
}
bool screen_reader_has_braille() {
	uint64_t features;
	return sr_state(features) && (features & PRISM_BACKEND_SUPPORTS_BRAILLE) != 0;
}
bool screen_reader_is_speaking() {
	uint64_t features;
	if (!sr_state(features)) return false;
	if (!(features & PRISM_BACKEND_SUPPORTS_IS_SPEAKING)) return false; // From the cache: for the backends that cannot report it (orca) this keeps the query free of blocking calls entirely.
	lock_guard<mutex> lock(sr().mutex);
	if (!sr().backend) return false;
	try {
		bool speaking = false;
		return prism_backend_is_speaking(sr().backend, &speaking) == PRISM_OK && speaking;
	} catch (...) {
		sr_release(); // The backend that threw is not trustworthy.
		return false;
	}
}
// The output calls are fire and forget: enqueueing is all they do, so navigation is never slowed by the reader's pace. Two safety nets run elsewhere: prism's availability poll keeps the cache correct proactively, and the worker re-speaks any message whose delivery failed through speech dispatcher, so a message is only reported as unsent (false) when no reader was installed at the moment of the call.
bool screen_reader_output(const std::string& text, bool interrupt) {
	if (text.empty()) return false;
	uint64_t features;
	if (!sr_state_wait(features) || !(features & PRISM_BACKEND_SUPPORTS_SPEAK)) return false;
	lock_guard<mutex> lock(sr().mutex);
	sr_job job;
	job.action = sr_job::kind::output;
	job.text = text;
	job.interrupt = interrupt;
	sr_enqueue_locked(job);
	return true;
}
bool screen_reader_speak(const std::string& text, bool interrupt) {
	if (text.empty()) return false;
	uint64_t features;
	if (!sr_state_wait(features) || !(features & PRISM_BACKEND_SUPPORTS_SPEAK)) return false;
	lock_guard<mutex> lock(sr().mutex);
	sr_job job;
	job.action = sr_job::kind::speak;
	job.text = text;
	job.interrupt = interrupt;
	sr_enqueue_locked(job);
	return true;
}
bool screen_reader_braille(const std::string& text) {
	if (text.empty()) return false;
	uint64_t features;
	if (!sr_state_wait(features) || !(features & PRISM_BACKEND_SUPPORTS_BRAILLE)) return false;
	lock_guard<mutex> lock(sr().mutex);
	sr_job job;
	job.action = sr_job::kind::braille;
	job.text = text;
	sr_enqueue_locked(job);
	return true;
}
bool screen_reader_silence() {
	uint64_t features;
	if (!sr_state_wait(features)) return false;
	if (!(features & PRISM_BACKEND_SUPPORTS_STOP)) return true; // A reader that cannot be stopped is silent as far as the script cares.
	lock_guard<mutex> lock(sr().mutex);
	sr_enqueue_locked(sr_job{sr_job::kind::stop, string(), false});
	return true;
}

// ============================================================================
// Platform registration
// ============================================================================

static const prism_engine_mapping g_engine_map[] = {
	{"speechd", PRISM_BACKEND_SPEECH_DISPATCHER},
	{"spiel", PRISM_BACKEND_SPIEL},
};
void register_native_tts() {
	// Must precede the first prism_get_context: the availability callback is a context creation parameter and is what keeps the reader cache correct without anyone probing.
	prism_set_availability_callback(sr_availability_changed, nullptr);
	prism_register_tts_engines(g_engine_map, sizeof(g_engine_map) / sizeof(g_engine_map[0]));
	// Kick an initial screen reader selection so the cached state is accurate by the time the script makes its first announcement.
	sr_request_select();
}

unsigned long long system_running_milliseconds() {
	struct timespec ts;
	if (clock_gettime(CLOCK_BOOTTIME, &ts) != 0) return 0;
	return (unsigned long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

#endif
