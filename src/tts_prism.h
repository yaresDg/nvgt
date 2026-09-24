/* tts_prism.h - header for the prism backed text to speech adapter
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

#pragma once
// This module adapts the prism library (https://github.com/ethindp/prism) to NVGT's engine based text to speech system.
// It is only compiled on platforms for which prism provides backends, for now that is windows and linux/bsd.

#if defined(_WIN32) || (!defined(__ANDROID__) && (defined(__linux__) || defined(__unix__) || defined(__FreeBSD__) || defined(__NetBSD__) || defined(__OpenBSD__) || defined(__DragonFly__)))

#include <prism.h>
#include <string>

// Registers one NVGT tts engine for every mapping in the caller's table that the connected prism build actually contains.
// Engines are registered under their long standing NVGT names (sapi5, speechd, etc) for script compatibility, regardless of which implementation is behind them.
// Each platform owns its own mapping table and calls this from its register_native_tts() implementation, listing the engines in the order they should be preferred.
struct prism_engine_mapping {
	const char *nvgt_name;
	PrismBackendId backend_id;
};
void prism_register_tts_engines(const prism_engine_mapping *map, size_t count);

// Shuts down the global prism context. Registered automatically with atexit the first time the context is created, so nothing needs to call this during normal application exit.
// Also safe to call explicitly at any point where no engine is actively being used, for example to force prism to release its resources early; the context is simply recreated on next use. Idempotent.
void prism_subsystem_shutdown();

// Returns the global prism context, creating it on first use. Thread safe.
PrismContext *prism_get_context();

// Shared screen reader plumbing used by the platform layers (win.cpp, linux.cpp). The strategy is platform specific (synchronous prism calls on windows; a worker thread on linux, where synchronous D-Bus calls from the script thread can deadlock against the reader), but selecting a backend and classifying its errors are the same everywhere.
// Attempts to select the highest priority screen reader backend that initializes, in the registry's own priority order, restricted to the caller's candidate ids. On success it copies the backend's feature mask and name and returns it, ownership transferring to the caller; otherwise it returns nullptr.
PrismBackend *prism_sr_select(PrismContext *ctx, const PrismBackendId *ids, size_t id_count, uint64_t &features, std::string &name);
// Reports whether an error returned by a screen reader backend call means the backend is permanently broken and must be released so the next call selects afresh; unimplemented operations and ordinary failures keep it.
bool prism_sr_error_invalidates(PrismError err);

#endif
