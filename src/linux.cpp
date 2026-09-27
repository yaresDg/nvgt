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
#include <time.h>
#include "linux.h"
#include "tts_prism.h"

// This platform's prioritized screen reader candidates, consumed by the shared screen reader layer in tts_prism.cpp. Orca only: the speech dispatcher backend remains available to scripts as a tts_voice engine, it just is not treated as a screen reader.
void prism_sr_platform_ids(const PrismBackendId *&ids, size_t &count) {
	static const PrismBackendId sr_ids[] = {
		PRISM_BACKEND_ORCA,
	};
	ids = sr_ids;
	count = sizeof(sr_ids) / sizeof(sr_ids[0]);
}

// ============================================================================
// Platform registration
// ============================================================================

static const prism_engine_mapping g_engine_map[] = {
	{"speechd", PRISM_BACKEND_SPEECH_DISPATCHER},
};
void register_native_tts() { prism_register_tts_engines(g_engine_map, sizeof(g_engine_map) / sizeof(g_engine_map[0])); }

unsigned long long system_running_milliseconds() {
	struct timespec ts;
	if (clock_gettime(CLOCK_BOOTTIME, &ts) != 0) return 0;
	return (unsigned long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

#endif
