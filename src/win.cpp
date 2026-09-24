/* win.cpp - code that only gets built when compiling for windows, things like prism engine registration, screen reader detection, keyhooks etc
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

#ifdef _WIN32 // Never include win.h outside of windows builds and this file can be lazy wildcarded into builds.
#define VC_EXTRALEAN

#define NOMINMAX
#include <windows.h>
#include <winuser.h>
#include <windows_process_watcher.h>
#include <thread>
#include <memory>
#include <atomic>
#include <chrono>
#include <mutex>
#include <fstream>
#include <ctime>
#include <iomanip>
#include "tts.h"
#include "tts_prism.h"
#include "win.h"

using namespace std;

static const prism_engine_mapping g_engine_map[] = {
	{"sapi5", PRISM_BACKEND_SAPI},
};
void register_native_tts() { prism_register_tts_engines(g_engine_map, sizeof(g_engine_map) / sizeof(g_engine_map[0])); }

// ============================================================================
// Screen reader layer
// ============================================================================

static PrismBackendId g_sr_backend_ids[] = {
	PRISM_BACKEND_NVDA,
	PRISM_BACKEND_JAWS,
	PRISM_BACKEND_ZDSR,
	PRISM_BACKEND_ZOOM_TEXT,
	PRISM_BACKEND_BOY_PC_READER,
	PRISM_BACKEND_PC_TALKER,
	PRISM_BACKEND_SENSE_READER,
	PRISM_BACKEND_SYSTEM_ACCESS,
	PRISM_BACKEND_WINDOW_EYES,
};

static mutex g_sr_mutex; // Prism backends are not thread safe and script calls can arrive from any thread.
static PrismBackend *g_sr_backend = nullptr;
static string g_sr_name;
static uint64_t g_sr_features = 0;

// Releases the cached screen reader backend. Must be called with g_sr_mutex held.
static void sr_release_locked() {
	if (!g_sr_backend) return;
	(void)prism_backend_stop(g_sr_backend);
	prism_backend_free(g_sr_backend);
	g_sr_backend = nullptr;
	g_sr_name.clear();
	g_sr_features = 0;
}

// Returns the cached reader, selecting one first if needed. Must be called with g_sr_mutex held.
static PrismBackend *sr_ensure_locked() {
	if (!g_sr_backend) {
		uint64_t features = 0;
		string name;
		PrismBackend *backend = prism_sr_select(prism_get_context(), g_sr_backend_ids, sizeof(g_sr_backend_ids) / sizeof(g_sr_backend_ids[0]), features, name);
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
	// No selecting just to answer this, and backends that cannot report it (orca) report not speaking.
	if (!g_sr_backend || !(g_sr_features & PRISM_BACKEND_SUPPORTS_IS_SPEAKING)) return false;
	try {
		bool speaking = false;
		return prism_backend_is_speaking(g_sr_backend, &speaking) == PRISM_OK && speaking;
	} catch (...) {
		sr_release_locked(); // The backend that threw is not trustworthy.
		return false;
	}
}
bool screen_reader_output(const std::string& text, bool interrupt) {
	if (text.empty()) return false;
	lock_guard<mutex> lock(g_sr_mutex);
	PrismBackend *backend = sr_ensure_locked();
	if (!backend || !(g_sr_features & PRISM_BACKEND_SUPPORTS_SPEAK)) return false;
	try {
		PrismError err = prism_backend_output(backend, text.c_str(), interrupt);
		if (prism_sr_error_invalidates(err)) sr_release_locked();
		return err == PRISM_OK;
	} catch (...) {
		sr_release_locked(); // The backend that threw is not trustworthy.
		return false;
	}
}
bool screen_reader_speak(const std::string& text, bool interrupt) {
	if (text.empty()) return false;
	lock_guard<mutex> lock(g_sr_mutex);
	PrismBackend *backend = sr_ensure_locked();
	if (!backend || !(g_sr_features & PRISM_BACKEND_SUPPORTS_SPEAK)) return false;
	try {
		PrismError err = prism_backend_speak(backend, text.c_str(), interrupt);
		if (prism_sr_error_invalidates(err)) sr_release_locked();
		return err == PRISM_OK;
	} catch (...) {
		sr_release_locked(); // The backend that threw is not trustworthy.
		return false;
	}
}
bool screen_reader_braille(const std::string& text) {
	if (text.empty()) return false;
	lock_guard<mutex> lock(g_sr_mutex);
	PrismBackend *backend = sr_ensure_locked();
	if (!backend || !(g_sr_features & PRISM_BACKEND_SUPPORTS_BRAILLE)) return false;
	try {
		PrismError err = prism_backend_braille(backend, text.c_str());
		if (prism_sr_error_invalidates(err)) sr_release_locked();
		return err == PRISM_OK;
	} catch (...) {
		sr_release_locked(); // The backend that threw is not trustworthy.
		return false;
	}
}
bool screen_reader_silence() {
	lock_guard<mutex> lock(g_sr_mutex);
	if (!g_sr_backend) return false;
	if (!(g_sr_features & PRISM_BACKEND_SUPPORTS_STOP)) return true; // A reader that cannot be stopped is silent as far as the script cares.
	try {
		PrismError err = prism_backend_stop(g_sr_backend);
		if (prism_sr_error_invalidates(err)) sr_release_locked();
		return err == PRISM_OK || err == PRISM_ERROR_NOT_SPEAKING; // Not speaking is a successful stop as far as tts_voice is concerned.
	} catch (...) {
		sr_release_locked(); // The backend that threw is not trustworthy.
		return false;
	}
}

// Thanks Quentin Cosendey (Universal Speech) for this jaws keyboard hook code as well as to male-srdiecko and silak for various improvements and fixes that have taken place since initial implementation.
bool altPressed = false;
bool capsPressed = false;
bool insertPressed = false;
static HHOOK g_keyhook_hHook = nullptr;
bool g_keyhook_active = false;
static std::unique_ptr<ProcessWatcher> g_process_watcher = nullptr;
static std::thread g_process_watcher_thread;
static std::atomic<bool> g_process_watcher_running{false};
static std::atomic<bool> g_window_focused{false};
static std::atomic<bool> g_jhookldr_process_running{false};
static bool g_keyhook_needs_uninstall = false;
static bool g_keyhook_needs_install = false;
// Used to control/reset various keys, usually insert, when toggling keyhook.
void send_keyboard_input(WORD vk_code, bool key_up) {
	INPUT input = {};
	input.type = INPUT_KEYBOARD;
	input.ki.wVk = vk_code;
	input.ki.dwFlags = key_up ? KEYEVENTF_KEYUP : 0;
	input.ki.time = 0;
	input.ki.dwExtraInfo = 0;
	SendInput(1, &input, sizeof(INPUT));
}
LRESULT CALLBACK HookKeyboardProc(int nCode, WPARAM wParam, LPARAM lParam) {
	if (nCode != HC_ACTION)
		return CallNextHookEx(g_keyhook_hHook, nCode, wParam, lParam);
	bool window_focused = g_window_focused.load();
	bool process_running = g_jhookldr_process_running.load();
	// Block keys only if both conditions are met:
	// 1. Our NVGT window is focused
	// 2. jhookldr.exe process is running
	if (!window_focused || !process_running)
		return CallNextHookEx(g_keyhook_hHook, nCode, wParam, lParam);
	PKBDLLHOOKSTRUCT p = reinterpret_cast<PKBDLLHOOKSTRUCT>(lParam);
	UINT vkCode = p->vkCode;
	bool altDown = p->flags & LLKHF_ALTDOWN;
	bool keyDown = (p->flags & LLKHF_UP) == 0;
	altPressed = altDown;
	if (vkCode != VK_CAPITAL && vkCode != VK_INSERT && (capsPressed || insertPressed))
		return CallNextHookEx(g_keyhook_hHook, nCode, wParam, lParam);
	switch (vkCode) {
		case VK_INSERT:
			insertPressed = keyDown;
			return CallNextHookEx(g_keyhook_hHook, nCode, wParam, lParam);
		case VK_CAPITAL:
			capsPressed = keyDown;
			return CallNextHookEx(g_keyhook_hHook, nCode, wParam, lParam);
		case VK_NUMLOCK:
		case VK_LCONTROL:
		case VK_RCONTROL:
		case VK_LSHIFT:
		case VK_RSHIFT:
			return CallNextHookEx(g_keyhook_hHook, nCode, wParam, lParam);
		default:
			return 0; // Block other keys when window is focused
	}
	return CallNextHookEx(g_keyhook_hHook, nCode, wParam, lParam);
}
void process_watcher_thread_func(const std::string& process_name) {
	g_process_watcher = std::make_unique<ProcessWatcher>(process_name);
	int elapsed_time = 10; // Start with fast checking
	bool found_process = false;
	while (g_process_watcher_running.load()) {
		std::this_thread::sleep_for(std::chrono::milliseconds(elapsed_time));
		// Check if hook is still installed.
		if (!g_keyhook_active) break;
		// Process watcher runs independently but only sends commands when window is focused
		if (!g_window_focused.load()) continue;
		// Check if process died.
		if (found_process && !g_process_watcher->monitor()) {
			elapsed_time = 60; // Slow down checking when process is dead
			found_process = false;
			g_jhookldr_process_running.store(false);
			g_keyhook_needs_uninstall = true;
			continue;
		} else if (!found_process) {
			if (g_process_watcher->find()) {
				found_process = true;
				elapsed_time = 10; // Speed up checking when process is found
				g_jhookldr_process_running.store(true);
				g_keyhook_needs_install = true;
			} else g_jhookldr_process_running.store(false);
		} else g_jhookldr_process_running.store(true);
	}
}
bool start_process_watcher(const std::string& process_name) {
	if (g_process_watcher_running.load()) {
		return false; // Already running
	}
	g_process_watcher_running.store(true);
	g_process_watcher_thread = std::thread(process_watcher_thread_func, process_name);
	return true;
}
void stop_process_watcher() {
	if (g_process_watcher_running.load()) {
		g_process_watcher_running.store(false);
		if (g_process_watcher_thread.joinable())
			g_process_watcher_thread.join();
		g_process_watcher.reset();
		g_jhookldr_process_running.store(false);
	}
}
// Function to only reinstall keyhook without affecting process watcher
bool reinstall_keyhook_only() {
	// Remove existing hook
	if (g_keyhook_hHook) {
		UnhookWindowsHookEx(g_keyhook_hHook);
		g_keyhook_hHook = nullptr;
	}
	// Install new hook
	g_keyhook_hHook = SetWindowsHookEx(WH_KEYBOARD_LL, HookKeyboardProc, GetModuleHandle(NULL), NULL);
	g_keyhook_active = true;
	if (g_keyhook_hHook) {
		send_keyboard_input(VK_INSERT, true);
		return true;
	} else {
		g_keyhook_active = false;
		return false;
	}
}
bool install_keyhook() {
	if (g_keyhook_hHook)
		uninstall_keyhook();
	g_keyhook_hHook = SetWindowsHookEx(WH_KEYBOARD_LL, HookKeyboardProc, GetModuleHandle(NULL), NULL);
	g_keyhook_active = true;
	if (g_keyhook_hHook) {
		send_keyboard_input(VK_INSERT, true);
		// Automatically start process watcher for jhookldr.exe (only on first install)
		if (!g_process_watcher_running.load())
			start_process_watcher("jhookldr.exe");
		return true;
	} else
		return false;
}
void remove_keyhook() {
	if (!g_keyhook_hHook)
		return;
	UnhookWindowsHookEx(g_keyhook_hHook);
	g_keyhook_hHook = nullptr;
}
void uninstall_keyhook() {
	remove_keyhook();
	stop_process_watcher();
	g_keyhook_active = false;
}
// Function to process keyhook commands in main thread.
void process_keyhook_commands() {
	if (g_keyhook_needs_uninstall) {
		g_keyhook_needs_uninstall = false;
		// Process died - actually uninstall hook via WinAPI to prevent JAWS from replacing it
		if (g_keyhook_hHook) {
			UnhookWindowsHookEx(g_keyhook_hHook);
			g_keyhook_hHook = nullptr;
		}
	}
	if (g_keyhook_needs_install) {
		g_keyhook_needs_install = false;
		// Process found - if window is focused and hook not installed, install it
		if (!g_keyhook_hHook && g_window_focused.load()) {
			g_keyhook_hHook = SetWindowsHookEx(WH_KEYBOARD_LL, HookKeyboardProc, GetModuleHandle(NULL), NULL);
			if (g_keyhook_hHook)
				send_keyboard_input(VK_INSERT, true);
		}
	}
}
void lost_window_focus_platform() {
	g_window_focused.store(false);
	if (g_keyhook_hHook) {
		UnhookWindowsHookEx(g_keyhook_hHook);
		g_keyhook_hHook = nullptr;
	}
}
void regained_window_focus_platform() {
	g_window_focused.store(true);
	if (!g_keyhook_hHook && g_keyhook_active) {
		g_keyhook_hHook = SetWindowsHookEx(WH_KEYBOARD_LL, HookKeyboardProc, GetModuleHandle(NULL), NULL);
		if (g_keyhook_hHook)
			send_keyboard_input(VK_INSERT, true);
	}
}

unsigned long long system_running_milliseconds() {
	return GetTickCount64();
}

#endif
