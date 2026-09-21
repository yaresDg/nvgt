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
#include <fstream>
#include <ctime>
#include <iomanip>
#include "tts_prism.h"
#include "win.h"

using namespace std;

void register_native_tts() { register_prism_tts_engines(); }

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
