/* tts_prism.h - header for the prism backed text to speech adapter
 *
 * NVGT - NonVisual Gaming Toolkit
 * Copyright (c) 2022 - 2026 Sam Tupy
 * https://nvgt.dev
 * This software is provided "as-is", without any express or implied warranty. In no event will the authors be held liable for any damages arising from the use of this software.
 * Permission is granted to anyone to use this software for any purpose, including commercial applications, and to alter it and redistribute it freely, subject to the following restrictions:
 * 1. The origin of this software must not be misrepresented; you must not claim that you wrote the original software. If you use this software in a product, an acknowledgment in the product documentation would be appreciated but is not required.
 * 2. Altered source versions must be plainly marked as such, and must not be misrepresented as being the original software.
 * 3. This notice may not be removed or altered from any source distribution.
*/

#pragma once
// This module adapts the prism library (https://github.com/ethindp/prism) to NVGT's engine based text to speech system.
// It is only compiled on platforms for which prism provides backends, for now that is windows and linux/bsd.

#if defined(_WIN32) || (!defined(__ANDROID__) && !defined(__APPLE__) && (defined(__linux__) || defined(__unix__) || defined(__FreeBSD__) || defined(__NetBSD__) || defined(__OpenBSD__) || defined(__DragonFly__)))

#include <prism.h>
#include <string>

struct prism_engine_mapping {
	const char *nvgt_name;
	PrismBackendId backend_id;
};
void prism_register_tts_engines(const prism_engine_mapping *map, size_t count);

PrismContext *prism_get_context();

void prism_sr_platform_ids(const PrismBackendId *&ids, size_t &count);

#endif
