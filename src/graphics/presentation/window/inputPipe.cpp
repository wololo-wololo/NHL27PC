// Scripted controller input for automated testing.
//
// With KYTY_INPUT_PIPE set ("1" for the default name, otherwise the pipe name), a named pipe
// \\.\pipe\<name> (default kyty-input) accepts one text command per line and answers each with
// one line ("ok", "pong" or "err <reason>"). Input goes to the keyboard player, so the emulator
// window does not need focus. Commands:
//   ping
//   tap <button> [hold_ms]        press, hold (default 120 ms), release
//   down <button> | up <button>
//   axis <LeftX|LeftY|RightX|RightY|L2|R2> <0..255>
//   release                       all buttons up, sticks centred, triggers released
//   shot <path>                   save the next guest flip as BMP ("ok WxH"); works minimized
// Buttons: cross circle square triangle up down left right l1 r1 l2 r2 l3 r3 options touchpad.

#include "graphics/presentation/window/inputPipe.h"

#include "common/logging/log.h"
#include "graphics/presentation/screenshot.h"
#include "libs/controller.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <sstream>
#include <string>
#include <thread>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

namespace Libs::Graphics {

namespace {

namespace Controller = Libs::Controller;

constexpr int ID = Controller::HOST_INPUT_CONTROLLER_ID;

struct ButtonName {
	const char* name;
	uint32_t    button;
};

constexpr ButtonName BUTTONS[] = {
    {"cross", Controller::PAD_BUTTON_CROSS},       {"x", Controller::PAD_BUTTON_CROSS},
    {"circle", Controller::PAD_BUTTON_CIRCLE},     {"o", Controller::PAD_BUTTON_CIRCLE},
    {"square", Controller::PAD_BUTTON_SQUARE},     {"triangle", Controller::PAD_BUTTON_TRIANGLE},
    {"up", Controller::PAD_BUTTON_UP},             {"down", Controller::PAD_BUTTON_DOWN},
    {"left", Controller::PAD_BUTTON_LEFT},         {"right", Controller::PAD_BUTTON_RIGHT},
    {"l1", Controller::PAD_BUTTON_L1},             {"r1", Controller::PAD_BUTTON_R1},
    {"l2", Controller::PAD_BUTTON_L2},             {"r2", Controller::PAD_BUTTON_R2},
    {"l3", Controller::PAD_BUTTON_L3},             {"r3", Controller::PAD_BUTTON_R3},
    {"options", Controller::PAD_BUTTON_OPTIONS},   {"touchpad", Controller::PAD_BUTTON_TOUCH_PAD},
};

constexpr uint32_t ALL_BUTTONS[] = {
    Controller::PAD_BUTTON_CROSS, Controller::PAD_BUTTON_CIRCLE, Controller::PAD_BUTTON_SQUARE,
    Controller::PAD_BUTTON_TRIANGLE, Controller::PAD_BUTTON_UP, Controller::PAD_BUTTON_DOWN,
    Controller::PAD_BUTTON_LEFT, Controller::PAD_BUTTON_RIGHT, Controller::PAD_BUTTON_L1,
    Controller::PAD_BUTTON_R1, Controller::PAD_BUTTON_L2, Controller::PAD_BUTTON_R2,
    Controller::PAD_BUTTON_L3, Controller::PAD_BUTTON_R3, Controller::PAD_BUTTON_OPTIONS,
    Controller::PAD_BUTTON_TOUCH_PAD,
};

std::atomic<bool> g_running {false};

bool FindButton(const std::string& name, uint32_t* button) {
	for (const auto& entry: BUTTONS) {
		if (name == entry.name) {
			*button = entry.button;
			return true;
		}
	}
	return false;
}

void SetButton(uint32_t button, bool down) {
	// Triggers are read as analog values as well as buttons (as in hostInput.cpp).
	if (button == Controller::PAD_BUTTON_L2) {
		Controller::SetAxis(ID, Controller::Axis::TriggerLeft, down ? 255 : 0);
	} else if (button == Controller::PAD_BUTTON_R2) {
		Controller::SetAxis(ID, Controller::Axis::TriggerRight, down ? 255 : 0);
	}
	Controller::SetButton(ID, button, down);
}

void ReleaseAll() {
	for (uint32_t button: ALL_BUTTONS) {
		SetButton(button, false);
	}
	Controller::SetAxis(ID, Controller::Axis::LeftX, 128);
	Controller::SetAxis(ID, Controller::Axis::LeftY, 128);
	Controller::SetRightStick(ID, 128, 128);
}

std::string Execute(const std::string& line) {
	if (line.rfind("shot ", 0) == 0) {
		std::string path = line.substr(5);
		path.erase(0, path.find_first_not_of(' '));
		std::string message;
		const bool  ok = CaptureScreenshot(path, 3000, &message);
		return (ok ? "ok " : "err ") + message;
	}
	std::istringstream in(line);
	std::string        command;
	std::string        argument;
	in >> command >> argument;
	auto lower = [](std::string& s) {
		std::transform(s.begin(), s.end(), s.begin(),
		               [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
	};
	lower(command);
	lower(argument);

	if (command.empty()) {
		return "err empty";
	}
	if (command == "ping") {
		return "pong";
	}
	if (command == "release") {
		ReleaseAll();
		return "ok";
	}
	if (command == "tap" || command == "down" || command == "up") {
		uint32_t button = 0;
		if (!FindButton(argument, &button)) {
			return "err unknown button '" + argument + "'";
		}
		if (command == "down" || command == "up") {
			SetButton(button, command == "down");
			return "ok";
		}
		int hold_ms = 120;
		in >> hold_ms;
		hold_ms = std::clamp(hold_ms, 16, 10000);
		SetButton(button, true);
		std::this_thread::sleep_for(std::chrono::milliseconds(hold_ms));
		SetButton(button, false);
		return "ok";
	}
	if (command == "axis") {
		int value = -1;
		in >> value;
		if (value < 0 || value > 255) {
			return "err axis value must be 0..255";
		}
		Controller::Axis axis {};
		if (argument == "leftx") {
			axis = Controller::Axis::LeftX;
		} else if (argument == "lefty") {
			axis = Controller::Axis::LeftY;
		} else if (argument == "rightx") {
			axis = Controller::Axis::RightX;
		} else if (argument == "righty") {
			axis = Controller::Axis::RightY;
		} else if (argument == "l2") {
			axis = Controller::Axis::TriggerLeft;
		} else if (argument == "r2") {
			axis = Controller::Axis::TriggerRight;
		} else {
			return "err unknown axis '" + argument + "'";
		}
		Controller::SetAxis(ID, axis, value);
		return "ok";
	}
	return "err unknown command '" + command + "'";
}

#ifdef _WIN32
void ServeClient(HANDLE pipe) {
	std::string pending;
	char        buffer[512];
	for (;;) {
		DWORD read = 0;
		if (!ReadFile(pipe, buffer, sizeof(buffer), &read, nullptr) || read == 0) {
			break;
		}
		pending.append(buffer, read);
		for (auto end = pending.find('\n'); end != std::string::npos; end = pending.find('\n')) {
			std::string line = pending.substr(0, end);
			pending.erase(0, end + 1);
			if (!line.empty() && line.back() == '\r') {
				line.pop_back();
			}
			const std::string reply   = Execute(line) + "\n";
			DWORD             written = 0;
			WriteFile(pipe, reply.data(), static_cast<DWORD>(reply.size()), &written, nullptr);
		}
	}
}

void Serve(std::string path) {
	// One thread per client so a client that stays connected never blocks the next one.
	while (g_running.load()) {
		HANDLE pipe = CreateNamedPipeA(path.c_str(), PIPE_ACCESS_DUPLEX,
		                               PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT,
		                               PIPE_UNLIMITED_INSTANCES, 4096, 4096, 0, nullptr);
		if (pipe == INVALID_HANDLE_VALUE) {
			LOGF("Input pipe: CreateNamedPipe failed (%lu)\n", GetLastError());
			return;
		}
		const bool connected =
		    ConnectNamedPipe(pipe, nullptr) != FALSE || GetLastError() == ERROR_PIPE_CONNECTED;
		if (!connected || !g_running.load()) {
			CloseHandle(pipe);
			continue;
		}
		std::thread([pipe] {
			ServeClient(pipe);
			DisconnectNamedPipe(pipe);
			CloseHandle(pipe);
		}).detach();
	}
}
#endif

} // namespace

void InputPipeStart() {
#ifdef _WIN32
	const char* value = std::getenv("KYTY_INPUT_PIPE");
	if (value == nullptr || *value == '\0' || g_running.exchange(true)) {
		return;
	}
	const std::string name = std::string(value) == "1" ? "kyty-input" : std::string(value);
	const std::string path = "\\\\.\\pipe\\" + name;
	LOGF("Input pipe: listening on %s\n", path.c_str());
	// The thread blocks in ConnectNamedPipe; it is detached and ends with the process.
	std::thread(Serve, path).detach();
#endif
}

void InputPipeStop() {
	g_running.store(false);
}

} // namespace Libs::Graphics
