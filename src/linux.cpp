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

// Prism's screen reader backends are synchronous: the orca backend in particular blocks in D-Bus calls until the reader answers, and the script thread must never make those calls itself: while it waits it cannot service our own window's accessibility tree, so a reader inspecting our window at that exact moment (orca does so on every keystroke) deadlocks against this process and gets killed by the desktop's watchdog. Every prism call below therefore runs on a dedicated worker thread: speech, braille and silence are fire and forget, and the queries are answered from the state the worker caches whenever it selects or loses a backend. While no reader is selected the queries also schedule a throttled reselection on the worker, so a reader that starts later is picked up on a following call.
static PrismBackendId g_sr_backend_ids[] = {
	PRISM_BACKEND_ORCA,
};

struct sr_job {
	enum class kind { select, speak, output, braille, stop };
	kind action;
	string text;
	bool interrupt = false;
};

static mutex g_sr_mutex; // One mutex guards the queue, the worker lifecycle and the cached state.
static condition_variable g_sr_cv;
static deque<sr_job> g_sr_queue;
static thread g_sr_worker;
static bool g_sr_worker_running = false;
static PrismBackend *g_sr_backend = nullptr; // Owned and only mutated by the worker.
static string g_sr_name;
static uint64_t g_sr_features = 0;
static chrono::steady_clock::time_point g_sr_last_select; // Throttles reselection while no reader is installed.

static void sr_worker_loop();
static void sr_stop_worker();

// Releases the cached screen reader backend. The stop is skipped while the process is going away: it is a synchronous D-Bus call that can wait out the death of the reader (GIO's default timeout is very long) with the main thread frozen inside exit(), which gets the reader watchdog killed while it crawls our dying window. When the whole process is going away anyway, freeing without stopping is enough.
static void sr_release(bool stop) {
	PrismBackend *backend;
	{
		lock_guard<mutex> lock(g_sr_mutex);
		backend = g_sr_backend;
		g_sr_backend = nullptr;
		g_sr_name.clear();
		g_sr_features = 0;
	}
	if (!backend) return;
	if (stop) (void)prism_backend_stop(backend);
	prism_backend_free(backend);
}

// Selects the highest priority reader backend that initializes, refreshing the cached state on success. Worker thread only: the registry scan makes synchronous D-Bus calls. Must not be called with g_sr_mutex held.
static PrismBackend *sr_select() {
	string name;
	uint64_t features = 0;
	PrismBackend *backend = prism_sr_select(prism_get_context(), g_sr_backend_ids, sizeof(g_sr_backend_ids) / sizeof(g_sr_backend_ids[0]), features, name);
	if (backend) {
		lock_guard<mutex> lock(g_sr_mutex);
		g_sr_backend = backend;
		g_sr_features = features;
		g_sr_name = std::move(name);
	}
	return backend;
}

// Returns the cached backend, selecting one first if needed. Worker thread only.
static PrismBackend *sr_backend_for_job() {
	{
		lock_guard<mutex> lock(g_sr_mutex);
		if (g_sr_backend) return g_sr_backend;
	}
	return sr_select();
}

// Performs one output job on the worker. A job whose call fails with an invalidating error drops the backend so the next job selects afresh.
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
	if (prism_sr_error_invalidates(err)) sr_release(true);
}

// The worker: drains the queue forever, keeping every prism call off the script thread.
static void sr_worker_loop() {
	for (;;) {
		sr_job job;
		{
			unique_lock<mutex> lock(g_sr_mutex);
			g_sr_cv.wait(lock, [] { return !g_sr_queue.empty() || !g_sr_worker_running; });
			if (g_sr_queue.empty()) return; // Stopped and fully drained.
			job = std::move(g_sr_queue.front());
			g_sr_queue.pop_front();
		}
		try {
			sr_perform(job);
		} catch (...) {
			sr_release(true); // Nothing that failed inside a prism call may escape this thread function: an uncaught exception in a std::thread aborts the whole process.
		}
	}
}

// Starts the worker once. Must be called with g_sr_mutex held.
static void sr_start_worker_locked() {
	if (g_sr_worker_running) return;
	static bool shutdown_registered = false;
	if (!shutdown_registered) {
		atexit(sr_stop_worker); // Joins the worker before the atexit registered by prism_get_context tears the prism context down.
		shutdown_registered = true;
	}
	g_sr_worker_running = true;
	g_sr_worker = thread(sr_worker_loop);
}

// Joins the worker, dropping anything still queued. Registered with atexit and called from the application shutdown path.
static void sr_stop_worker() {
	thread worker;
	{
		lock_guard<mutex> lock(g_sr_mutex);
		if (!g_sr_worker_running) return;
		g_sr_worker_running = false;
		g_sr_queue.clear(); // Nothing good comes from announcing while the process is going away.
		worker = std::move(g_sr_worker);
	}
	g_sr_cv.notify_all();
	worker.join();
	sr_release(false);
}

// Schedules a selection on the worker, throttled to one attempt per second: with no reader installed, scanning the registry on every call would probe its backends nonstop. Must be called with g_sr_mutex held.
static void sr_request_select_locked() {
	if (chrono::steady_clock::now() - g_sr_last_select < chrono::milliseconds(1000)) return;
	g_sr_last_select = chrono::steady_clock::now();
	sr_start_worker_locked();
	if (g_sr_queue.size() >= 32) g_sr_queue.pop_front(); // If the reader cannot keep up, the oldest pending work is dropped in favor of the newest.
	g_sr_queue.push_back(sr_job{sr_job::kind::select, string(), false});
	g_sr_cv.notify_one();
}

// Copies the cached feature mask for the script visible queries and, while no reader is selected, schedules the throttled reselection that picks up a reader started later.
static bool sr_state(uint64_t &features) {
	lock_guard<mutex> lock(g_sr_mutex);
	if (g_sr_backend) {
		features = g_sr_features;
		return true;
	}
	sr_request_select_locked();
	return false;
}

// Must be called with g_sr_mutex held, after sr_state confirmed a reader with the wanted feature.
static void sr_enqueue_locked(const sr_job &job) {
	if (g_sr_queue.size() >= 32) g_sr_queue.pop_front();
	g_sr_queue.push_back(job);
	g_sr_cv.notify_one();
}

static void sr_request_select() {
	lock_guard<mutex> lock(g_sr_mutex);
	sr_request_select_locked();
}

bool screen_reader_load() {
	uint64_t features;
	return sr_state(features);
}
void screen_reader_unload() {
	sr_stop_worker();
}
std::string screen_reader_detect() {
	uint64_t features;
	if (!sr_state(features)) return std::string();
	lock_guard<mutex> lock(g_sr_mutex);
	return g_sr_name;
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
	lock_guard<mutex> lock(g_sr_mutex);
	if (!g_sr_backend) return false;
	try {
		bool speaking = false;
		return prism_backend_is_speaking(g_sr_backend, &speaking) == PRISM_OK && speaking;
	} catch (...) {
		sr_release(true); // The backend that threw is not trustworthy.
		return false;
	}
}
bool screen_reader_output(const std::string& text, bool interrupt) {
	if (text.empty()) return false;
	uint64_t features;
	if (!sr_state(features) || !(features & PRISM_BACKEND_SUPPORTS_SPEAK)) return false;
	lock_guard<mutex> lock(g_sr_mutex);
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
	if (!sr_state(features) || !(features & PRISM_BACKEND_SUPPORTS_SPEAK)) return false;
	lock_guard<mutex> lock(g_sr_mutex);
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
	if (!sr_state(features) || !(features & PRISM_BACKEND_SUPPORTS_BRAILLE)) return false;
	lock_guard<mutex> lock(g_sr_mutex);
	sr_job job;
	job.action = sr_job::kind::braille;
	job.text = text;
	sr_enqueue_locked(job);
	return true;
}
bool screen_reader_silence() {
	uint64_t features;
	if (!sr_state(features)) return false;
	if (!(features & PRISM_BACKEND_SUPPORTS_STOP)) return true; // A reader that cannot be stopped is silent as far as the script cares.
	lock_guard<mutex> lock(g_sr_mutex);
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
