/* tts_prism.cpp - adapter between the prism speech library and NVGT's engine based text to speech system
 * Prism (https://github.com/ethindp/prism) provides the actual platform speech backends (SAPI, OneCore, Speech Dispatcher, etc).
 * This file only translates between prism's C API and NVGT's tts_engine interface, everything else (voice aggregation, scheduling, playback of synthesized audio) keeps belonging to tts.cpp.
 * Engines are registered under their long standing NVGT names so that existing scripts keep working, see the engine map at the bottom of this file.
 *
 * NVGT - NonVisual Gaming Toolkit
 * Copyright (c) 2026 Sam Tupy
 * https://nvgt.dev
 * This software is provided "as-is", without any express or implied warranty. In no event will the authors be held liable for any damages arising from the use of this software.
 * Permission is granted to anyone to use this software for any purpose, including commercial applications, and to alter it and redistribute it freely, subject to the following restrictions:
 * 1. The origin of this software must not be misrepresented; you must not claim that you wrote the original software. If you use this software in a product, an acknowledgment in the product documentation would be appreciated but is not required.
 * 2. Altered source versions must be plainly marked as such, and must not be misrepresented as being the original software.
 * 3. This notice may not be removed or altered from any source distribution.
*/

#if defined(_WIN32) || (!defined(__ANDROID__) && (defined(__linux__) || defined(__unix__) || defined(__FreeBSD__) || defined(__NetBSD__) || defined(__OpenBSD__) || defined(__DragonFly__)))

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdlib>
#include <deque>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <vector>
#if defined(_WIN32)
// prism.h includes windows.h, which without this defines min/max as macros that then break std::max calls inside reactphysics3d headers pulled in by tts.h below.
#define NOMINMAX
#endif
#include <prism.h>
#include "tts.h"
#include "tts_prism.h"

using namespace std;

// Global prism context, shared by every prism backed engine and created on first use.
static mutex g_prism_mutex;
static PrismContext *g_prism_context = nullptr;

// Defined in the screen reader layer below: they route prism's availability transitions into the screen reader worker.
static void PRISM_CALL sr_availability_callback(void *userdata, PrismBackendId backend, const char *name, bool available);
static void PRISM_CALL sr_availability_baseline_callback(void *userdata);

static PrismContext *prism_get_context() {
	lock_guard<mutex> lock(g_prism_mutex);
	if (!g_prism_context) {
		PrismConfig config = prism_config_init();
		config.availability_callback = sr_availability_callback;
		config.availability_poll_interval_ms = 100;
		config.availability_debounce_samples = 2;
		config.availability_backoff_max_ms = 0;
		config.availability_baseline_callback = sr_availability_baseline_callback;
		g_prism_context = prism_init(&config);
		if (g_prism_context) atexit(prism_subsystem_shutdown);
	}
	return g_prism_context;
}

void prism_subsystem_shutdown() {
	PrismContext *ctx;
	{
		lock_guard<mutex> lock(g_prism_mutex);
		ctx = g_prism_context;
		g_prism_context = nullptr;
	}
	prism_shutdown(ctx);
}

// Receives synthesized audio from prism_backend_speak_to_memory. Prism may invoke it any number of times and from any thread, but guarantees no further calls once speak_to_memory returns, so nothing more than an accumulator is needed.
struct pcm_accumulator {
	vector<float> samples;
	size_t channels = 0;
	size_t sample_rate = 0;
};

static void PRISM_CALL pcm_accumulate(void *userdata, const float *samples, size_t sample_count, size_t channels, size_t sample_rate) {
	pcm_accumulator *accumulator = (pcm_accumulator *)userdata;
	if (!accumulator->channels) {
		accumulator->channels = channels;
		accumulator->sample_rate = sample_rate;
	}
	accumulator->samples.insert(accumulator->samples.end(), samples, samples + sample_count);
}

class prism_tts_engine : public tts_engine_impl {
	PrismBackend *backend;
	uint64_t features;
	mutable mutex backend_mutex; // Prism backends are not thread safe and engine calls can arrive from any script thread.

	bool has_feature(uint64_t feature) const { return (features & feature) != 0; }

public:
	prism_tts_engine(const string &name, PrismBackendId backend_id) : tts_engine_impl(name), backend(nullptr), features(0) {
		PrismContext *ctx = prism_get_context();
		if (!ctx) throw runtime_error("prism could not be initialized");
		backend = prism_registry_create(ctx, backend_id);
		if (!backend) throw runtime_error("prism backend " + name + " could not be created");
		PrismError err = prism_backend_initialize(backend);
		if (err != PRISM_OK) {
			prism_backend_free(backend);
			backend = nullptr;
			throw runtime_error("prism backend " + name + " failed to initialize: " + prism_error_string(err));
		}
		features = prism_backend_get_features(backend);
	}

	~prism_tts_engine() override {
		if (!backend) return;
		(void)prism_backend_stop(backend);
		prism_backend_free(backend);
	}

	bool is_available() override { return backend != nullptr; }

	tts_pcm_generation_state get_pcm_generation_state() override {
		return has_feature(PRISM_BACKEND_SUPPORTS_SPEAK_TO_MEMORY) ? PCM_PREFERRED : PCM_UNSUPPORTED;
	}

	tts_audio_data* speak_to_pcm(const string &text) override {
		if (!backend || text.empty() || !has_feature(PRISM_BACKEND_SUPPORTS_SPEAK_TO_MEMORY)) return nullptr;
		lock_guard<mutex> lock(backend_mutex);
		pcm_accumulator accumulator;
		PrismError err = prism_backend_speak_to_memory(backend, text.c_str(), pcm_accumulate, &accumulator);
		if (err != PRISM_OK || accumulator.samples.empty() || !accumulator.channels || !accumulator.sample_rate) return nullptr;
		// Convert to 16 bit PCM, the format tts_voice and NVGT's audio pipeline expect from tts engines.
		unsigned int size_in_bytes = (unsigned int)(accumulator.samples.size() * 2);
		int16_t *data = (int16_t *)malloc(size_in_bytes);
		if (!data) return nullptr;
		for (size_t i = 0; i < accumulator.samples.size(); i++) {
			float sample = accumulator.samples[i];
			if (sample > 1.0f) sample = 1.0f;
			else if (sample < -1.0f) sample = -1.0f;
			data[i] = (int16_t)lrintf(sample * 32767.0f);
		}
		return new tts_audio_data(this, data, size_in_bytes, (unsigned int)accumulator.sample_rate, (unsigned int)accumulator.channels, 16);
	}

	bool speak(const string &text, bool interrupt, bool blocking) override {
		if (!backend || text.empty() || !has_feature(PRISM_BACKEND_SUPPORTS_SPEAK)) return false;
		lock_guard<mutex> lock(backend_mutex);
		PrismError err = prism_backend_speak(backend, text.c_str(), interrupt);
		if (err != PRISM_OK) return false;
		if (blocking && has_feature(PRISM_BACKEND_SUPPORTS_IS_SPEAKING)) {
			bool speaking = true;
			while (speaking && prism_backend_is_speaking(backend, &speaking) == PRISM_OK)
				this_thread::sleep_for(chrono::milliseconds(10));
		}
		return true;
	}

	bool is_speaking() override {
		if (!backend || !has_feature(PRISM_BACKEND_SUPPORTS_IS_SPEAKING)) return false;
		lock_guard<mutex> lock(backend_mutex);
		bool speaking = false;
		return prism_backend_is_speaking(backend, &speaking) == PRISM_OK && speaking;
	}

	bool stop() override {
		if (!backend || !has_feature(PRISM_BACKEND_SUPPORTS_STOP)) return tts_engine_impl::stop();
		lock_guard<mutex> lock(backend_mutex);
		PrismError err = prism_backend_stop(backend);
		return err == PRISM_OK || err == PRISM_ERROR_NOT_SPEAKING; // Not speaking is a successful stop as far as tts_voice is concerned.
	}

	// Prism normalizes speech parameters to 0..1 with the default at 0.5, so the engine ranges reported here simply are that range and the standard range_convert_midpoint conversion performed by tts_voice maps NVGT's -10..10 rate/pitch and -100..0 volume scales onto it losslessly.
	bool get_rate_range(float& minimum, float& midpoint, float& maximum) override {
		if (!backend || !has_feature(PRISM_BACKEND_SUPPORTS_SET_RATE)) return false;
		minimum = 0; midpoint = 0.5f; maximum = 1;
		return true;
	}
	bool get_pitch_range(float& minimum, float& midpoint, float& maximum) override {
		if (!backend || !has_feature(PRISM_BACKEND_SUPPORTS_SET_PITCH)) return false;
		minimum = 0; midpoint = 0.5f; maximum = 1;
		return true;
	}
	bool get_volume_range(float& minimum, float& midpoint, float& maximum) override {
		if (!backend || !has_feature(PRISM_BACKEND_SUPPORTS_SET_VOLUME)) return false;
		minimum = 0; midpoint = 0.5f; maximum = 1;
		return true;
	}
	float get_rate() override {
		if (!backend || !has_feature(PRISM_BACKEND_SUPPORTS_GET_RATE)) return tts_engine_impl::get_rate();
		lock_guard<mutex> lock(backend_mutex);
		float rate = 0;
		return prism_backend_get_rate(backend, &rate) == PRISM_OK ? rate : 0;
	}
	float get_pitch() override {
		if (!backend || !has_feature(PRISM_BACKEND_SUPPORTS_GET_PITCH)) return tts_engine_impl::get_pitch();
		lock_guard<mutex> lock(backend_mutex);
		float pitch = 0;
		return prism_backend_get_pitch(backend, &pitch) == PRISM_OK ? pitch : 0;
	}
	float get_volume() override {
		if (!backend || !has_feature(PRISM_BACKEND_SUPPORTS_GET_VOLUME)) return tts_engine_impl::get_volume();
		lock_guard<mutex> lock(backend_mutex);
		float volume = 0;
		return prism_backend_get_volume(backend, &volume) == PRISM_OK ? volume : 0;
	}
	void set_rate(float rate) override {
		if (!backend || !has_feature(PRISM_BACKEND_SUPPORTS_SET_RATE)) return;
		lock_guard<mutex> lock(backend_mutex);
		(void)prism_backend_set_rate(backend, clamp(rate, 0.0f, 1.0f));
	}
	void set_pitch(float pitch) override {
		if (!backend || !has_feature(PRISM_BACKEND_SUPPORTS_SET_PITCH)) return;
		lock_guard<mutex> lock(backend_mutex);
		(void)prism_backend_set_pitch(backend, clamp(pitch, 0.0f, 1.0f));
	}
	void set_volume(float volume) override {
		if (!backend || !has_feature(PRISM_BACKEND_SUPPORTS_SET_VOLUME)) return;
		lock_guard<mutex> lock(backend_mutex);
		(void)prism_backend_set_volume(backend, clamp(volume, 0.0f, 1.0f));
	}

	int get_voice_count() override {
		if (!backend || !has_feature(PRISM_BACKEND_SUPPORTS_COUNT_VOICES)) return tts_engine_impl::get_voice_count();
		lock_guard<mutex> lock(backend_mutex);
		size_t count = 0;
		if (prism_backend_count_voices(backend, &count) != PRISM_OK) return tts_engine_impl::get_voice_count();
		return (int)count;
	}
	string get_voice_name(int index) override {
		if (!backend || index < 0 || !has_feature(PRISM_BACKEND_SUPPORTS_GET_VOICE_NAME)) return tts_engine_impl::get_voice_name(index);
		lock_guard<mutex> lock(backend_mutex);
		const char *name = nullptr;
		if (prism_backend_get_voice_name(backend, (size_t)index, &name) != PRISM_OK || !name) return "";
		return string(name);
	}
	string get_voice_language(int index) override {
		if (!backend || index < 0 || !has_feature(PRISM_BACKEND_SUPPORTS_GET_VOICE_LANGUAGE)) return "";
		lock_guard<mutex> lock(backend_mutex);
		const char *language = nullptr;
		if (prism_backend_get_voice_language(backend, (size_t)index, &language) != PRISM_OK || !language) return "";
		return string(language);
	}
	bool set_voice(int voice) override {
		if (!backend || voice < 0 || !has_feature(PRISM_BACKEND_SUPPORTS_SET_VOICE)) return tts_engine_impl::set_voice(voice);
		lock_guard<mutex> lock(backend_mutex);
		return prism_backend_set_voice(backend, (size_t)voice) == PRISM_OK;
	}
	int get_current_voice() override {
		if (!backend || !has_feature(PRISM_BACKEND_SUPPORTS_GET_VOICE)) return tts_engine_impl::get_current_voice();
		lock_guard<mutex> lock(backend_mutex);
		size_t voice = 0;
		if (prism_backend_get_voice(backend, &voice) != PRISM_OK) return tts_engine_impl::get_current_voice();
		return (int)voice;
	}
};

struct prism_engine_mapping {
	const char *nvgt_name;
	PrismBackendId backend_id;
};
static const prism_engine_mapping g_prism_engine_map[] = {
	{"sapi5", PRISM_BACKEND_SAPI},
	{"onecore", PRISM_BACKEND_ONE_CORE},
	{"speechd", PRISM_BACKEND_SPEECH_DISPATCHER},
	{"spiel", PRISM_BACKEND_SPIEL},
};

static void sr_request_select(); // Defined in the screen reader layer below.

void register_prism_tts_engines() {
	PrismContext *ctx = prism_get_context();
	if (!ctx) return;
	for (const prism_engine_mapping &mapping : g_prism_engine_map) {
		if (!prism_registry_exists(ctx, mapping.backend_id)) continue;
		string name = mapping.nvgt_name;
		PrismBackendId backend_id = mapping.backend_id;
		tts_engine_register(name, [name, backend_id]() -> shared_ptr<tts_engine> { return make_shared<prism_tts_engine>(name, backend_id); });
	}
	// Kick an initial screen reader selection right away on the worker thread, so the snapshot the script thread reads is already accurate by the time the script makes its first announcement.
	sr_request_select();
}

// ============================================================================
// Screen reader layer
// ============================================================================

#if defined(_WIN32) || !defined(__APPLE__)

#if defined(_WIN32)
PrismBackendId g_sr_backend_ids[] = {
	PRISM_BACKEND_NVDA,
	PRISM_BACKEND_JAWS,
	PRISM_BACKEND_UIA, 
	PRISM_BACKEND_ZDSR,
	PRISM_BACKEND_ZOOM_TEXT,
	PRISM_BACKEND_BOY_PC_READER,
	PRISM_BACKEND_PC_TALKER,
	PRISM_BACKEND_SENSE_READER,
	PRISM_BACKEND_SYSTEM_ACCESS,
	PRISM_BACKEND_WINDOW_EYES,
	PRISM_BACKEND_ORCA, // Only initializes when running under wine with the Linux side reachable.
};
#else
PrismBackendId g_sr_backend_ids[] = {
	PRISM_BACKEND_ORCA,
};
#endif

// Prism's screen reader backends are synchronous: the Orca backend in particular blocks in D-Bus calls until the reader answers. The script thread must never make those calls itself: while it waits it cannot service our own window's accessibility tree, so a reader inspecting our window at that exact moment (Orca does so on every keystroke) deadlocks against this process and gets killed by the desktop's watchdog. Every prism call below therefore runs on a dedicated worker thread: speech, braille and silence are fire and forget, and the queries are answered from a snapshot the worker refreshes whenever it selects or loses a backend. The cost is that the bool returned by the output functions means "handed to the reader" rather than "the reader confirmed the presentation", and that a reader which just died is reported unavailable until the next throttled reselection.
static mutex g_sr_backend_mutex; // Serializes every prism backend call: normally the worker's, plus the rare direct queries.
static PrismBackend *g_sr_backend = nullptr; // Owned by the worker thread, guarded by g_sr_backend_mutex.
static bool g_sr_shutting_down = false; // Set once the process is exiting: from that point on releasing a backend must not make synchronous calls into the reader. Guarded by g_sr_backend_mutex.

// Snapshot of the currently selected backend that the script thread reads without ever touching prism. Guarded by g_sr_state_mutex.
enum class sr_state { unknown, ready, failed };
static mutex g_sr_state_mutex;
static condition_variable g_sr_state_cv; // Wakes snapshot readers that are waiting out a transition (see sr_reader_available).
static sr_state g_sr_state = sr_state::unknown;
static string g_sr_name;
static uint64_t g_sr_features = 0;

struct sr_job {
	enum class kind { select, release, speak, output, braille, stop };
	kind action;
	string text;
	bool interrupt = false;
};

static mutex g_sr_queue_mutex;
static condition_variable g_sr_queue_cv;
static deque<sr_job> g_sr_queue;
static bool g_sr_worker_running = false; // Guarded by g_sr_queue_mutex.
static thread g_sr_worker;
// Number of select jobs queued but not yet run by the worker. Snapshot readers consult this before waiting out a transition: waiting only makes sense while a selection that could flip the snapshot is actually in flight. Benign races are fine; the counter is a hint, the snapshot itself stays the source of truth.
static atomic<int> g_sr_pending_selects{0};

static void sr_worker_loop();
static void sr_stop_worker();
static void sr_worker_run(const sr_job &job);
static bool sr_error_invalidates_backend(PrismError err);
static void sr_request_select();

// Marks the snapshot as having no usable reader. Must be called with g_sr_state_mutex held.
static void sr_state_clear_locked(sr_state state) {
	g_sr_state = state;
	g_sr_name.clear();
	g_sr_features = 0;
	g_sr_state_cv.notify_all();
}

// Releases the cached screen reader backend. Must be called with g_sr_backend_mutex held.
static void sr_release_backend_locked() {
	if (!g_sr_backend) return;
	// The stop is skipped while the process is exiting: it is a synchronous D-Bus call that can wait out the death of the reader (GIO's default timeout is very long) with the main thread frozen inside exit(), which freezes our own accessibility tree in turn and gets the reader watchdog killed while it crawls our dying window. When the whole process is going away anyway, freeing without stopping is enough.
	if (!g_sr_shutting_down) (void)prism_backend_stop(g_sr_backend);
	prism_backend_free(g_sr_backend);
	g_sr_backend = nullptr;
}

// Starts the screen reader worker thread once. Idempotent.
static void sr_start_worker() {
	lock_guard<mutex> lock(g_sr_queue_mutex);
	if (g_sr_worker_running) return;
	static bool shutdown_registered = false;
	if (!shutdown_registered) {
		atexit(sr_stop_worker); // Joins the worker before the atexit registered by prism_get_context tears the prism context down.
		shutdown_registered = true;
	}
	g_sr_worker_running = true;
	g_sr_worker = thread(sr_worker_loop);
}

// Joins the worker, dropping anything still queued. Registered with atexit on worker startup.
static void sr_stop_worker() {
	{
		lock_guard<mutex> lock(g_sr_queue_mutex);
		if (!g_sr_worker_running) return;
		g_sr_worker_running = false;
		g_sr_queue.clear(); // Nothing good comes from announcing while the process is going away, and the loop's way out releases the backend without talking to the reader.
	}
	g_sr_queue_cv.notify_all();
	if (g_sr_worker.joinable()) g_sr_worker.join();
}

// Enqueues work for the worker without ever blocking the caller. If the reader cannot keep up, the oldest pending output is dropped in favor of the newest. Everything is dropped silently once the process is shutting down (the flag write races benignly; it is set exactly once).
static void sr_enqueue(sr_job::kind action, string text = string(), bool interrupt = false) {
	if (g_sr_shutting_down) return;
	sr_start_worker();
	sr_job job;
	job.action = action;
	job.text = std::move(text);
	job.interrupt = interrupt;
	if (action == sr_job::kind::select) g_sr_pending_selects.fetch_add(1, std::memory_order_release);
	{
		lock_guard<mutex> lock(g_sr_queue_mutex);
		if (g_sr_queue.size() >= 32) g_sr_queue.pop_front();
		g_sr_queue.push_back(std::move(job));
	}
	g_sr_queue_cv.notify_one();
}

// Queues a backend selection on the worker. Prism's availability poll thread is what drives selections during the process lifetime (it reports the reader starting and stopping), so this only serves startup and the baseline reconciliation. Safe from any thread.
static void sr_request_select() {
	sr_enqueue(sr_job::kind::select);
}

static bool sr_id_is_screen_reader(PrismBackendId id) {
	for (PrismBackendId sr_id : g_sr_backend_ids) if (id == sr_id) return true;
	return false;
}

// Invoked from prism's internal availability poll thread when a backend starts or stops being usable. It must return quickly and must not block, so all it does is route the transition into the worker's queue: an available reader gets selected, a vanished one gets released and taken out of the snapshot.
static void PRISM_CALL sr_availability_callback(void *userdata, PrismBackendId backend, const char *name, bool available) {
	(void)userdata;
	(void)name;
	if (!sr_id_is_screen_reader(backend)) return;
	sr_enqueue(available ? sr_job::kind::select : sr_job::kind::release);
}

// Invoked once prism's poll thread has established its availability baseline. The direct selection made at startup happened before that baseline, so a reader that came or went in between is absorbed silently; reselecting here reconciles the snapshot with reality. Same constraints as the availability callback.
static void PRISM_CALL sr_availability_baseline_callback(void *userdata) {
	(void)userdata;
	sr_request_select();
}

// Attempts to select the highest priority screen reader backend that initializes, in the registry's own priority order, refreshing the script visible snapshot with the result. Must be called with g_sr_backend_mutex held.
// This loop cannot be delegated to prism_registry_acquire_best: a frozen registry is always a superset of the global one, so best-first selection would fall through to plain TTS backends, and the screen reader layer must never speak through plain TTS. Restricting the candidates to the reader ids here is the smallest correct equivalent.
static void sr_select_backend_locked() {
	sr_release_backend_locked();
	PrismContext *ctx = prism_get_context();
	if (!ctx) {
		lock_guard<mutex> lock(g_sr_state_mutex);
		sr_state_clear_locked(sr_state::failed);
		return;
	}
	size_t count = prism_registry_count(ctx);
	for (size_t i = 0; i < count; i++) {
		PrismBackendId id = prism_registry_id_at(ctx, i);
		if (!sr_id_is_screen_reader(id)) continue;
		PrismBackend *backend = prism_registry_create(ctx, id);
		if (!backend) continue;
		if (prism_backend_initialize(backend) != PRISM_OK) {
			prism_backend_free(backend);
			continue;
		}
		uint64_t features = prism_backend_get_features(backend);
		if (!(features & PRISM_BACKEND_IS_SUPPORTED_AT_RUNTIME)) {
			prism_backend_free(backend);
			continue;
		}
		g_sr_backend = backend;
		lock_guard<mutex> lock(g_sr_state_mutex);
		const char *name = prism_backend_name(backend);
		g_sr_state = sr_state::ready;
		g_sr_name = name ? name : "";
		g_sr_features = features;
		g_sr_state_cv.notify_all();
		return;
	}
	lock_guard<mutex> lock(g_sr_state_mutex);
	sr_state_clear_locked(sr_state::failed);
}

// Drops a backend that failed in any way, including the exceptions prism's backends can let escape through their C API (prism is not exception safe: the Orca backend for one can propagate std::out_of_range while parsing a reply from a reader that died mid exchange). Must be called without g_sr_backend_mutex held.
static void sr_worker_recover() {
	{
		lock_guard<mutex> lock(g_sr_backend_mutex);
		sr_release_backend_locked();
	}
	lock_guard<mutex> state_lock(g_sr_state_mutex);
	sr_state_clear_locked(sr_state::unknown);
}

// The worker: drains the queue forever, running every prism call away from the script thread.
static void sr_worker_loop() {
	for (;;) {
		sr_job job;
		{
			unique_lock<mutex> lock(g_sr_queue_mutex);
			g_sr_queue_cv.wait(lock, [] { return !g_sr_queue.empty() || !g_sr_worker_running; });
			if (g_sr_queue.empty()) break; // Stopped and fully drained.
			job = std::move(g_sr_queue.front());
			g_sr_queue.pop_front();
		}
		try {
			if (job.action == sr_job::kind::select || job.action == sr_job::kind::release) {
				lock_guard<mutex> lock(g_sr_backend_mutex);
				if (job.action == sr_job::kind::select) {
					sr_select_backend_locked();
				} else {
					sr_release_backend_locked();
					lock_guard<mutex> state_lock(g_sr_state_mutex);
					sr_state_clear_locked(sr_state::unknown); // A release, for example one triggered by prism's availability poll, must also take the snapshot down with it.
				}
			} else {
				sr_worker_run(job);
			}
		} catch (...) {
			sr_worker_recover(); // Nothing that failed inside a prism call may escape this thread function: an uncaught exception in a std::thread aborts the whole process.
		}
		if (job.action == sr_job::kind::select) g_sr_pending_selects.fetch_sub(1, std::memory_order_release); // After the catch too: a failed select still stops counting as pending.
	}
	try {
		lock_guard<mutex> lock(g_sr_backend_mutex);
		sr_release_backend_locked();
	} catch (...) {
	} // Same rule on the way out: at worst the backend leaks during process teardown.
}

// Runs one output job, selecting a backend first if needed.
static void sr_worker_run(const sr_job &job) {
	lock_guard<mutex> lock(g_sr_backend_mutex);
	if (!g_sr_backend) {
		sr_select_backend_locked();
		if (!g_sr_backend) return;
	}
	PrismError err;
	switch (job.action) {
		case sr_job::kind::speak: err = prism_backend_speak(g_sr_backend, job.text.c_str(), job.interrupt); break;
		case sr_job::kind::output: err = prism_backend_output(g_sr_backend, job.text.c_str(), job.interrupt); break;
		case sr_job::kind::braille: err = prism_backend_braille(g_sr_backend, job.text.c_str()); break;
		case sr_job::kind::stop: err = prism_backend_stop(g_sr_backend); break;
		default: return;
	}
	if (sr_error_invalidates_backend(err)) {
		sr_release_backend_locked();
		lock_guard<mutex> state_lock(g_sr_state_mutex);
		sr_state_clear_locked(sr_state::unknown); // The reader died or its environment vanished: let the next activity reselect.
	}
}

// A screen reader dying or its environment vanishing (NVDA exited, UIA window destroyed) leaves the cached backend permanently broken, so those errors invalidate it and the next call detects afresh. Unimplemented operations are normal per backend and keep the cache.
static bool sr_error_invalidates_backend(PrismError err) {
	// SpeakFailure is included because a reader that vanishes mid run (orca exited, its bus name went away) surfaces exactly as the Glib::Error the Orca backend translates into SpeakFailure. Without it the cached snapshot would keep claiming a working reader and swallow every announcement, instead of going unavailable so the speech routing in the scripts can fall back to the TTS engine. EnteredUndefinedState is included because prism's own documentation says the caller should re-initialize from scratch after it.
	return err == PRISM_ERROR_BACKEND_NOT_AVAILABLE || err == PRISM_ERROR_INTERNAL || err == PRISM_ERROR_NOT_INITIALIZED || err == PRISM_ERROR_SPEAK_FAILURE || err == PRISM_ERROR_BACKEND_ENTERED_UNDEFINED_STATE;
}

// Snapshot accessor: reports whether a reader is currently selected and copies its features. Selections are driven by prism's availability callbacks, so this never requests one; it only reads. When the snapshot is not ready but a selection is actually in flight or queued (the reader was just (re)started and its availability callback has fired), it waits a bounded moment for the worker to resolve it: that is what makes the first announcement after a reader (re)start go through the reader instead of falling back to the TTS engine. Without a pending selection it returns immediately, so a stable "no reader" state costs callers nothing beyond the mutex.
static bool sr_reader_available(uint64_t &features) {
	{
		lock_guard<mutex> lock(g_sr_state_mutex);
		if (g_sr_state == sr_state::ready) {
			features = g_sr_features;
			return true;
		}
		if (g_sr_pending_selects.load(std::memory_order_acquire) == 0) return false;
	}
	unique_lock<mutex> lock(g_sr_state_mutex);
	g_sr_state_cv.wait_for(lock, chrono::milliseconds(300), [&] { return g_sr_state == sr_state::ready; });
	if (g_sr_state != sr_state::ready) return false;
	features = g_sr_features;
	return true;
}

bool screen_reader_load() {
	uint64_t features;
	return sr_reader_available(features);
}
void screen_reader_unload() {
	{
		lock_guard<mutex> lock(g_sr_backend_mutex);
		g_sr_shutting_down = true; // Only ever called from the application shutdown path: from here on releasing the backend must not talk to the reader synchronously.
	}
	{
		lock_guard<mutex> lock(g_sr_state_mutex);
		sr_state_clear_locked(sr_state::unknown);
	}
	sr_enqueue(sr_job::kind::release);
}
std::string screen_reader_detect() {
	uint64_t features;
	if (!sr_reader_available(features)) return std::string();
	lock_guard<mutex> lock(g_sr_state_mutex);
	return g_sr_name;
}
bool screen_reader_has_speech() {
	uint64_t features;
	return sr_reader_available(features) && (features & PRISM_BACKEND_SUPPORTS_SPEAK) != 0;
}
bool screen_reader_has_braille() {
	uint64_t features;
	return sr_reader_available(features) && (features & PRISM_BACKEND_SUPPORTS_BRAILLE) != 0;
}
bool screen_reader_is_speaking() {
	uint64_t features;
	if (!sr_reader_available(features)) return false;
	if (!(features & PRISM_BACKEND_SUPPORTS_IS_SPEAKING)) return false; // From the snapshot: for the backends that cannot report it (Orca) this keeps the query free of blocking calls entirely.
	lock_guard<mutex> lock(g_sr_backend_mutex);
	if (!g_sr_backend) return false;
	try {
		bool speaking = false;
		return prism_backend_is_speaking(g_sr_backend, &speaking) == PRISM_OK && speaking;
	} catch (...) {
		sr_release_backend_locked(); // Already holding g_sr_backend_mutex: the backend that threw is not trustworthy.
		lock_guard<mutex> state_lock(g_sr_state_mutex);
		sr_state_clear_locked(sr_state::unknown);
		return false;
	}
}
bool screen_reader_output(const std::string& text, bool interrupt) {
	if (text.empty()) return false;
	uint64_t features;
	if (!sr_reader_available(features) || !(features & PRISM_BACKEND_SUPPORTS_SPEAK)) return false;
	sr_enqueue(sr_job::kind::output, text, interrupt);
	return true;
}
bool screen_reader_speak(const std::string& text, bool interrupt) {
	if (text.empty()) return false;
	uint64_t features;
	if (!sr_reader_available(features) || !(features & PRISM_BACKEND_SUPPORTS_SPEAK)) return false;
	sr_enqueue(sr_job::kind::speak, text, interrupt);
	return true;
}
bool screen_reader_braille(const std::string& text) {
	if (text.empty()) return false;
	uint64_t features;
	if (!sr_reader_available(features) || !(features & PRISM_BACKEND_SUPPORTS_BRAILLE)) return false;
	sr_enqueue(sr_job::kind::braille, text);
	return true;
}
bool screen_reader_silence() {
	uint64_t features;
	if (!sr_reader_available(features)) return false;
	if (!(features & PRISM_BACKEND_SUPPORTS_STOP)) return true; // A reader that cannot be stopped is silent as far as the script cares.
	sr_enqueue(sr_job::kind::stop);
	return true;
}

#endif // Screen reader layer

#endif
