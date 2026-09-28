/* tts_prism.cpp - adapter between the prism speech library and NVGT's engine based text to speech system
 * Prism (https://github.com/ethindp/prism) provides the actual platform speech and backends (SAPI, Speech Dispatcher, etc).
 * This file translates between prism's C API and NVGT's tts_engine interface and hosts the entire screen reader communication layer.
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

#if defined(_WIN32) || (!defined(__ANDROID__) && !defined(__APPLE__) && (defined(__linux__) || defined(__unix__) || defined(__FreeBSD__) || defined(__NetBSD__) || defined(__OpenBSD__) || defined(__DragonFly__)))

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
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

static mutex g_prism_mutex;
static PrismContext *g_prism_context = nullptr;

static void PRISM_CALL prism_sr_availability_changed(void *userdata, PrismBackendId backend, const char *name, bool available);

PrismContext *prism_get_context() {
	lock_guard<mutex> lock(g_prism_mutex);
	if (!g_prism_context) {
		PrismConfig config = prism_config_init();
		config.availability_callback = prism_sr_availability_changed;
		config.availability_poll_interval_ms = 100;
		config.availability_debounce_samples = 1;
		config.availability_backoff_max_ms = 0;
		config.availability_auto_power_manage = true;
		g_prism_context = prism_init(&config);
	}
	return g_prism_context;
}

// Receives synthesized audio from prism_backend_speak_to_memory
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
	mutable mutex backend_mutex;

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
		lock_guard<mutex> lock(backend_mutex);
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
		return err == PRISM_OK || err == PRISM_ERROR_NOT_SPEAKING;
	}

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

void prism_register_tts_engines(const prism_engine_mapping *map, size_t count) {
	PrismContext *ctx = prism_get_context();
	if (!ctx) return;
	for (size_t i = 0; i < count; i++) {
		const prism_engine_mapping &mapping = map[i];
		if (!prism_registry_exists(ctx, mapping.backend_id)) continue;
		string name = mapping.nvgt_name;
		PrismBackendId backend_id = mapping.backend_id;
		tts_engine_register(name, [name, backend_id]() -> shared_ptr<tts_engine> { return make_shared<prism_tts_engine>(name, backend_id); });
	}
}

// ============================================================================
// Screen reader layer
// ============================================================================

static bool prism_sr_error_invalidates(PrismError err) {
	return err == PRISM_ERROR_BACKEND_NOT_AVAILABLE || err == PRISM_ERROR_INTERNAL || err == PRISM_ERROR_NOT_INITIALIZED || err == PRISM_ERROR_SPEAK_FAILURE || err == PRISM_ERROR_BACKEND_ENTERED_UNDEFINED_STATE;
}

static PrismBackend *prism_sr_select(PrismContext *ctx, const PrismBackendId *ids, size_t id_count, PrismBackendId &selected_id, uint64_t &features, std::string &name) {
	for (size_t i = 0; i < id_count; i++) {
		if (!prism_registry_exists(ctx, ids[i])) continue;
		PrismBackend *backend = prism_registry_create(ctx, ids[i]);
		if (!backend) continue;
		uint64_t mask = prism_backend_get_features(backend);
		if (!(mask & PRISM_BACKEND_IS_SUPPORTED_AT_RUNTIME)) {
			prism_backend_free(backend);
			continue;
		}
		if (prism_backend_initialize(backend) != PRISM_OK) {
			prism_backend_free(backend);
			continue;
		}
		const char *backend_name = prism_backend_name(backend);
		selected_id = ids[i];
		features = mask;
		name = backend_name ? backend_name : "";
		return backend;
	}
	return nullptr;
}

static mutex g_sr_mutex;
static PrismBackend *g_sr_backend = nullptr;
static PrismBackendId g_sr_id = PRISM_BACKEND_INVALID;
static string g_sr_name;
static uint64_t g_sr_features = 0;

static void sr_release_locked() {
	if (!g_sr_backend) return;
	(void)prism_backend_stop(g_sr_backend);
	prism_backend_free(g_sr_backend);
	g_sr_backend = nullptr;
	g_sr_id = PRISM_BACKEND_INVALID;
	g_sr_name.clear();
	g_sr_features = 0;
}

static void PRISM_CALL prism_sr_availability_changed(void *, PrismBackendId backend, const char *, bool available) {
	const PrismBackendId *ids = nullptr;
	size_t id_count = 0;
	prism_sr_platform_ids(ids, id_count);
	bool is_screen_reader = false;
	for (size_t i = 0; i < id_count && !is_screen_reader; i++) is_screen_reader = ids[i] == backend;
	if (!is_screen_reader) return;
	lock_guard<mutex> lock(g_sr_mutex);
	if (available || !g_sr_backend || backend != g_sr_id) return;
	sr_release_locked();
}

static PrismBackend *sr_ensure_locked() {
	if (!g_sr_backend) {
		const PrismBackendId *ids = nullptr;
		size_t id_count = 0;
		prism_sr_platform_ids(ids, id_count);
		uint64_t features = 0;
		string name;
		PrismBackend *backend = prism_sr_select(prism_get_context(), ids, id_count, g_sr_id, features, name);
		if (backend) {
			g_sr_backend = backend;
			g_sr_features = features;
			g_sr_name = name;
		}
	}
	return g_sr_backend;
}

bool screen_reader_load() {
	lock_guard<mutex> lock(g_sr_mutex);
	return sr_ensure_locked() != nullptr;
}
void screen_reader_unload() {
	lock_guard<mutex> lock(g_sr_mutex);
	sr_release_locked();
}
std::string screen_reader_detect() {
	lock_guard<mutex> lock(g_sr_mutex);
	if (!sr_ensure_locked()) return std::string();
	return g_sr_name;
}
bool screen_reader_has_speech() {
	lock_guard<mutex> lock(g_sr_mutex);
	return sr_ensure_locked() != nullptr && (g_sr_features & PRISM_BACKEND_SUPPORTS_SPEAK) != 0;
}
bool screen_reader_has_braille() {
	lock_guard<mutex> lock(g_sr_mutex);
	return sr_ensure_locked() != nullptr && (g_sr_features & PRISM_BACKEND_SUPPORTS_BRAILLE) != 0;
}
bool screen_reader_is_speaking() {
	lock_guard<mutex> lock(g_sr_mutex);
	if (!g_sr_backend || !(g_sr_features & PRISM_BACKEND_SUPPORTS_IS_SPEAKING)) return false;
	bool speaking = false;
	return prism_backend_is_speaking(g_sr_backend, &speaking) == PRISM_OK && speaking;
}
bool screen_reader_output(const std::string& text, bool interrupt) {
	if (text.empty()) return false;
	lock_guard<mutex> lock(g_sr_mutex);
	PrismBackend *backend = sr_ensure_locked();
	if (!backend || !(g_sr_features & PRISM_BACKEND_SUPPORTS_SPEAK)) return false;
	PrismError err = prism_backend_output(backend, text.c_str(), interrupt);
	if (prism_sr_error_invalidates(err)) sr_release_locked();
	return err == PRISM_OK;
}
bool screen_reader_speak(const std::string& text, bool interrupt) {
	if (text.empty()) return false;
	lock_guard<mutex> lock(g_sr_mutex);
	PrismBackend *backend = sr_ensure_locked();
	if (!backend || !(g_sr_features & PRISM_BACKEND_SUPPORTS_SPEAK)) return false;
	PrismError err = prism_backend_speak(backend, text.c_str(), interrupt);
	if (prism_sr_error_invalidates(err)) sr_release_locked();
	return err == PRISM_OK;
}
bool screen_reader_braille(const std::string& text) {
	if (text.empty()) return false;
	lock_guard<mutex> lock(g_sr_mutex);
	PrismBackend *backend = sr_ensure_locked();
	if (!backend || !(g_sr_features & PRISM_BACKEND_SUPPORTS_BRAILLE)) return false;
	PrismError err = prism_backend_braille(backend, text.c_str());
	if (prism_sr_error_invalidates(err)) sr_release_locked();
	return err == PRISM_OK;
}
bool screen_reader_silence() {
	lock_guard<mutex> lock(g_sr_mutex);
	if (!g_sr_backend) return false;
	if (!(g_sr_features & PRISM_BACKEND_SUPPORTS_STOP)) return true;
	PrismError err = prism_backend_stop(g_sr_backend);
	if (prism_sr_error_invalidates(err)) sr_release_locked();
	return err == PRISM_OK || err == PRISM_ERROR_NOT_SPEAKING;
}

#endif
