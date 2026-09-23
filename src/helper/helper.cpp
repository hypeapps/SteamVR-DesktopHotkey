// SteamVR-DesktopHotkey - helper process
//
// Started by the driver together with SteamVR (--background). Registers a global hotkey with the
// standard RegisterHotKey() API (no keyboard hooks, no input injection, nothing touches games) and
// talks to SteamVR through the public OpenVR API:
//   - dashboard closed -> IVROverlay::ShowDashboard(<desktop overlay key>)
//   - dashboard open   -> signal the driver to press the extra headset input, a head-aimed click
//                         that closes the dashboard when the gaze is not on the dashboard panel
//
// Command line:
//   desktop_hotkey_helper.exe --background   run in background (used by the driver)
//   desktop_hotkey_helper.exe --toggle       toggle once and exit (for AutoHotkey, Stream Deck, ...)
//   desktop_hotkey_helper.exe --open         open the dashboard on the desktop view
//   desktop_hotkey_helper.exe --close        close the dashboard if it is open
//   desktop_hotkey_helper.exe --press        press the headset input directly (diagnostics)
//   desktop_hotkey_helper.exe --probe        print dashboard/overlay diagnostics
//
// SPDX-License-Identifier: MIT

#include "../common/common.h"

#include <openvr.h>

#include <shellapi.h>
#include <tlhelp32.h>

#include <algorithm>
#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <ctime>
#include <cwctype>
#include <string>
#include <vector>

namespace {

    constexpr const wchar_t* kDefaultHotkey = L"Ctrl+Alt+D";
    constexpr const wchar_t* kDefaultOverlayKey = L"system.desktop.1";
    constexpr int kHotkeyId = 1;

    std::wstring g_driverRoot;
    std::wstring g_ini;
    std::wstring g_logPath;
    bool g_console = false;

    // ---------------------------------------------------------------------------------------------
    // Logging (file in %LOCALAPPDATA%\SteamVR-DesktopHotkey\helper.log, plus console when available)
    // ---------------------------------------------------------------------------------------------
    void InitLogging() {
        wchar_t base[MAX_PATH]{};
        if (GetEnvironmentVariableW(L"LOCALAPPDATA", base, MAX_PATH) > 0) {
            std::wstring dir = std::wstring(base) + L"\\SteamVR-DesktopHotkey";
            CreateDirectoryW(dir.c_str(), nullptr);
            g_logPath = dir + L"\\helper.log";

            // Keep the log small: start fresh when it grows over 1 MB.
            WIN32_FILE_ATTRIBUTE_DATA info{};
            if (GetFileAttributesExW(g_logPath.c_str(), GetFileExInfoStandard, &info) &&
                (info.nFileSizeHigh > 0 || info.nFileSizeLow > 1024 * 1024)) {
                DeleteFileW(g_logPath.c_str());
            }
        }
        g_console = AttachConsole(ATTACH_PARENT_PROCESS) != FALSE;
    }

    void Print(const char* format, ...) {
        char message[2048];
        va_list args;
        va_start(args, format);
        vsnprintf(message, sizeof(message), format, args);
        va_end(args);

        if (g_console) {
            HANDLE out = GetStdHandle(STD_OUTPUT_HANDLE);
            if (out && out != INVALID_HANDLE_VALUE) {
                DWORD written = 0;
                WriteFile(out, message, static_cast<DWORD>(strlen(message)), &written, nullptr);
                WriteFile(out, "\r\n", 2, &written, nullptr);
            }
        }

        if (!g_logPath.empty()) {
            FILE* file = _wfopen(g_logPath.c_str(), L"a");
            if (file) {
                SYSTEMTIME t;
                GetLocalTime(&t);
                fprintf(file,
                        "%04u-%02u-%02u %02u:%02u:%02u [%lu] %s\n",
                        t.wYear,
                        t.wMonth,
                        t.wDay,
                        t.wHour,
                        t.wMinute,
                        t.wSecond,
                        GetCurrentProcessId(),
                        message);
                fclose(file);
            }
        }
    }

    // ---------------------------------------------------------------------------------------------
    // Hotkey parsing: "Ctrl+Alt+D", "Ctrl+Shift+F12", "Win+Alt+Home", "Ctrl+Alt+0x7B", ...
    // ---------------------------------------------------------------------------------------------
    std::wstring Lower(std::wstring s) {
        std::transform(s.begin(), s.end(), s.begin(), [](wchar_t c) { return static_cast<wchar_t>(towlower(c)); });
        return s;
    }

    UINT ParseKeyName(const std::wstring& rawName) {
        const std::wstring name = Lower(rawName);
        if (name.empty()) {
            return 0;
        }

        // Raw virtual-key code, e.g. 0x7B
        if (name.size() > 2 && name[0] == L'0' && name[1] == L'x') {
            return static_cast<UINT>(wcstoul(name.c_str() + 2, nullptr, 16));
        }

        // Letters and digits
        if (name.size() == 1 && ((name[0] >= L'a' && name[0] <= L'z') || (name[0] >= L'0' && name[0] <= L'9'))) {
            return static_cast<UINT>(towupper(name[0]));
        }

        // F1..F24
        if (name.size() >= 2 && name[0] == L'f' && iswdigit(name[1])) {
            const int n = _wtoi(name.c_str() + 1);
            if (n >= 1 && n <= 24) {
                return static_cast<UINT>(VK_F1 + n - 1);
            }
        }

        // Numpad0..Numpad9
        if (name.size() == 7 && name.rfind(L"numpad", 0) == 0 && iswdigit(name[6])) {
            return static_cast<UINT>(VK_NUMPAD0 + (name[6] - L'0'));
        }

        struct Named {
            const wchar_t* name;
            UINT vk;
        };
        static const Named kNamed[] = {
            {L"space", VK_SPACE},         {L"tab", VK_TAB},
            {L"enter", VK_RETURN},        {L"return", VK_RETURN},
            {L"esc", VK_ESCAPE},          {L"escape", VK_ESCAPE},
            {L"backspace", VK_BACK},      {L"insert", VK_INSERT},
            {L"ins", VK_INSERT},          {L"delete", VK_DELETE},
            {L"del", VK_DELETE},          {L"home", VK_HOME},
            {L"end", VK_END},             {L"pageup", VK_PRIOR},
            {L"pgup", VK_PRIOR},          {L"pagedown", VK_NEXT},
            {L"pgdn", VK_NEXT},           {L"up", VK_UP},
            {L"down", VK_DOWN},           {L"left", VK_LEFT},
            {L"right", VK_RIGHT},         {L"pause", VK_PAUSE},
            {L"scrolllock", VK_SCROLL},   {L"printscreen", VK_SNAPSHOT},
            {L"numpadadd", VK_ADD},       {L"numpadsubtract", VK_SUBTRACT},
            {L"numpadmultiply", VK_MULTIPLY}, {L"numpaddivide", VK_DIVIDE},
            {L"numpaddecimal", VK_DECIMAL},
        };
        for (const auto& entry : kNamed) {
            if (name == entry.name) {
                return entry.vk;
            }
        }

        // Any other single printable character (e.g. "-", "=", "[", "`") on the current layout.
        if (rawName.size() == 1) {
            const SHORT scan = VkKeyScanW(rawName[0]);
            if (scan != -1) {
                return static_cast<UINT>(LOBYTE(scan));
            }
        }
        return 0;
    }

    bool ParseHotkey(const std::wstring& text, UINT& modifiers, UINT& vk) {
        modifiers = 0;
        vk = 0;

        std::vector<std::wstring> parts;
        size_t start = 0;
        while (start <= text.size()) {
            size_t plus = text.find(L'+', start);
            // Allow "+" itself as the key, e.g. "Ctrl++"
            if (plus == start && plus + 1 == text.size()) {
                parts.push_back(L"+");
                break;
            }
            if (plus == std::wstring::npos) {
                plus = text.size();
            }
            parts.push_back(dh::Trim(text.substr(start, plus - start)));
            start = plus + 1;
        }

        for (const auto& part : parts) {
            const std::wstring p = Lower(part);
            if (p.empty()) {
                continue;
            }
            if (p == L"ctrl" || p == L"control") {
                modifiers |= MOD_CONTROL;
            } else if (p == L"alt") {
                modifiers |= MOD_ALT;
            } else if (p == L"shift") {
                modifiers |= MOD_SHIFT;
            } else if (p == L"win" || p == L"windows") {
                modifiers |= MOD_WIN;
            } else if (vk == 0) {
                vk = ParseKeyName(part);
                if (vk == 0) {
                    return false;
                }
            } else {
                return false; // more than one non-modifier key
            }
        }
        return vk != 0;
    }

    // ---------------------------------------------------------------------------------------------
    // OpenVR
    // ---------------------------------------------------------------------------------------------
    bool g_vrInitialized = false;

    // True while vrserver has our driver loaded (the driver owns this named event).
    bool IsDriverLoaded() {
        HANDLE event = OpenEventW(SYNCHRONIZE, FALSE, dh::kPressEventName);
        if (!event) {
            return false;
        }
        CloseHandle(event);
        return true;
    }

    // Connects as a background app: never starts SteamVR, and fails with "no server" (121) while
    // SteamVR is not running or still starting up.
    vr::EVRInitError ConnectToSteamVR() {
        vr::EVRInitError error = vr::VRInitError_None;
        vr::VR_Init(&error, vr::VRApplication_Background);
        g_vrInitialized = error == vr::VRInitError_None;
        return error;
    }

    bool IsElevated(HANDLE process) {
        HANDLE token = nullptr;
        if (!OpenProcessToken(process, TOKEN_QUERY, &token)) {
            return false;
        }
        TOKEN_ELEVATION elevation{};
        DWORD size = 0;
        const bool ok = GetTokenInformation(token, TokenElevation, &elevation, sizeof(elevation), &size) != FALSE;
        CloseHandle(token);
        return ok && elevation.TokenIsElevated != 0;
    }

    void PrintEnvironment() {
        Print("Helper:      %s", IsElevated(GetCurrentProcess()) ? "elevated (admin)" : "not elevated");

        char runtime[MAX_PATH * 2]{};
        uint32_t required = 0;
        if (vr::VR_GetRuntimePath(runtime, sizeof(runtime), &required)) {
            Print("Runtime:     %s", runtime);
        } else {
            Print("Runtime:     NOT FOUND (VR_IsRuntimeInstalled=%d)", vr::VR_IsRuntimeInstalled() ? 1 : 0);
        }

        HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
        bool found = false;
        if (snapshot != INVALID_HANDLE_VALUE) {
            PROCESSENTRY32W entry{};
            entry.dwSize = sizeof(entry);
            for (BOOL more = Process32FirstW(snapshot, &entry); more; more = Process32NextW(snapshot, &entry)) {
                if (_wcsicmp(entry.szExeFile, L"vrserver.exe") != 0 && _wcsicmp(entry.szExeFile, L"vrmonitor.exe") != 0 &&
                    _wcsicmp(entry.szExeFile, L"vrcompositor.exe") != 0 && _wcsicmp(entry.szExeFile, L"vrdashboard.exe") != 0) {
                    continue;
                }
                found = true;
                std::string path = "(path not accessible)";
                const char* elevation = "unknown";
                HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, entry.th32ProcessID);
                if (process) {
                    wchar_t buffer[MAX_PATH * 2]{};
                    DWORD length = ARRAYSIZE(buffer);
                    if (QueryFullProcessImageNameW(process, 0, buffer, &length)) {
                        path = dh::Narrow(buffer);
                    }
                    elevation = IsElevated(process) ? "elevated" : "not elevated";
                    CloseHandle(process);
                }
                Print("Process:     %s pid %lu, %s, %s",
                      dh::Narrow(entry.szExeFile).c_str(),
                      entry.th32ProcessID,
                      elevation,
                      path.c_str());
            }
            CloseHandle(snapshot);
        }
        if (!found) {
            Print("Process:     no SteamVR processes running");
        }
    }

    void DisconnectFromSteamVR() {
        if (g_vrInitialized) {
            vr::VR_Shutdown();
            g_vrInitialized = false;
        }
    }

    std::string OverlayKey() {
        // An empty key means: just press the headset input (opens the dashboard on its last page).
        return dh::Narrow(dh::IniString(g_ini, L"dashboard", L"desktop_overlay_key", kDefaultOverlayKey));
    }

    bool PressHeadsetInput() {
        HANDLE event = OpenEventW(EVENT_MODIFY_STATE, FALSE, dh::kPressEventName);
        if (!event) {
            Print("Cannot reach the desktop_hotkey driver (is it installed and enabled in SteamVR?). Error %lu",
                  GetLastError());
            return false;
        }
        SetEvent(event);
        CloseHandle(event);
        return true;
    }

    bool IsDesktopOverlayActive(const std::string& key) {
        if (key.empty()) {
            return false;
        }
        vr::VROverlayHandle_t handle = vr::k_ulOverlayHandleInvalid;
        if (vr::VROverlay()->FindOverlay(key.c_str(), &handle) != vr::VROverlayError_None) {
            return false;
        }
        return vr::VROverlay()->IsActiveDashboardOverlay(handle);
    }

    bool OverlayExists(const std::string& key) {
        vr::VROverlayHandle_t handle = vr::k_ulOverlayHandleInvalid;
        return !key.empty() && vr::VROverlay()->FindOverlay(key.c_str(), &handle) == vr::VROverlayError_None;
    }

    void OpenDesktop() {
        const std::string configured = OverlayKey();
        if (configured.empty()) {
            Print("Opening dashboard (headset input)");
            PressHeadsetInput();
            return;
        }

        // Use the configured key; if SteamVR does not know it, try the keys used by known SteamVR versions.
        std::string key = configured;
        if (!OverlayExists(key)) {
            for (const char* candidate : {"system.desktop.1", "valve.steam.desktop"}) {
                if (OverlayExists(candidate)) {
                    Print("Overlay '%s' not found, using '%s'", configured.c_str(), candidate);
                    key = candidate;
                    break;
                }
            }
        }
        Print("Opening dashboard on '%s'", key.c_str());
        vr::VROverlay()->ShowDashboard(key.c_str());
    }

    void CloseDashboard() {
        Print("Closing dashboard (headset input, gaze must not be on the dashboard panel)");
        PressHeadsetInput();
    }

    void Toggle() {
        auto* overlay = vr::VROverlay();
        if (!overlay) {
            Print("IVROverlay not available");
            return;
        }

        if (!overlay->IsDashboardVisible()) {
            OpenDesktop();
            return;
        }

        // Dashboard already open. By default close it; optionally jump to the desktop first
        // when another dashboard page is showing.
        const std::wstring whenOpen = Lower(dh::IniString(g_ini, L"dashboard", L"when_open", L"close"));
        if (whenOpen == L"desktop_then_close" && !IsDesktopOverlayActive(OverlayKey())) {
            OpenDesktop();
            return;
        }
        CloseDashboard();
    }

    // ---------------------------------------------------------------------------------------------
    // Modes
    // ---------------------------------------------------------------------------------------------
    int RunBackground() {
        Print("Helper build %s", dh::kBuildId);
        HANDLE mutex = CreateMutexW(nullptr, TRUE, dh::kHelperMutexName);
        if (!mutex || GetLastError() == ERROR_ALREADY_EXISTS) {
            Print("Another helper instance is already running, exiting");
            if (mutex) {
                CloseHandle(mutex);
            }
            return dh::kExitOk;
        }

        if (dh::IniInt(g_ini, L"hotkey", L"enabled", 1) == 0) {
            Print("Hotkey disabled in config.ini ([hotkey] enabled=0), exiting");
            return dh::kExitOk;
        }

        HANDLE quitEvent = OpenEventW(SYNCHRONIZE, FALSE, dh::kQuitEventName);

        // Wait for vrserver to accept our connection.
        for (int attempt = 1;; attempt++) {
            const auto error = ConnectToSteamVR();
            if (error == vr::VRInitError_None) {
                break;
            }
            if (attempt == 1 || attempt % 30 == 0) {
                Print("Waiting for SteamVR (attempt %d): %s", attempt, vr::VR_GetVRInitErrorAsEnglishDescription(error));
            }
            if (error != vr::VRInitError_Init_NoServerForBackgroundApp && error != vr::VRInitError_Init_HmdNotFound &&
                error != vr::VRInitError_Init_HmdNotFoundPresenceFailed) {
                return dh::kExitRetry;
            }
            if (quitEvent ? WaitForSingleObject(quitEvent, 2000) == WAIT_OBJECT_0 : (Sleep(2000), false)) {
                return dh::kExitOk;
            }
        }
        Print("Connected to SteamVR");

        const std::wstring hotkeyText = dh::IniString(g_ini, L"hotkey", L"keys", kDefaultHotkey);
        UINT modifiers = 0, vk = 0;
        if (!ParseHotkey(hotkeyText, modifiers, vk)) {
            Print("Invalid hotkey '%s' in config.ini", dh::Narrow(hotkeyText).c_str());
            DisconnectFromSteamVR();
            return dh::kExitError;
        }
        if (!RegisterHotKey(nullptr, kHotkeyId, modifiers | MOD_NOREPEAT, vk)) {
            Print("RegisterHotKey('%s') failed with error %lu - the combination is probably used by another program",
                  dh::Narrow(hotkeyText).c_str(),
                  GetLastError());
            DisconnectFromSteamVR();
            return dh::kExitError;
        }
        Print("Hotkey registered: %s", dh::Narrow(hotkeyText).c_str());

        bool exiting = false;
        while (!exiting) {
            DWORD handleCount = quitEvent ? 1 : 0;
            const DWORD wait = MsgWaitForMultipleObjects(handleCount, &quitEvent, FALSE, 250, QS_ALLINPUT);
            if (quitEvent && wait == WAIT_OBJECT_0) {
                Print("Driver requested exit");
                break;
            }

            MSG msg;
            while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
                if (msg.message == WM_HOTKEY && msg.wParam == kHotkeyId) {
                    Toggle();
                } else if (msg.message == WM_QUIT) {
                    exiting = true;
                }
                TranslateMessage(&msg);
                DispatchMessageW(&msg);
            }

            vr::VREvent_t event{};
            while (vr::VRSystem() && vr::VRSystem()->PollNextEvent(&event, sizeof(event))) {
                if (event.eventType == vr::VREvent_Quit) {
                    Print("SteamVR is quitting");
                    vr::VRSystem()->AcknowledgeQuit_Exiting();
                    exiting = true;
                }
            }
        }

        UnregisterHotKey(nullptr, kHotkeyId);
        DisconnectFromSteamVR();
        if (quitEvent) {
            CloseHandle(quitEvent);
        }
        CloseHandle(mutex);
        return dh::kExitOk;
    }

    int RunPress() {
        // Presses the headset input without looking at the dashboard state: if the dashboard opens,
        // the input and its binding work; if nothing happens at all, the input never arrives.
        Print("Pressing the headset input (no dashboard logic)");
        return PressHeadsetInput() ? dh::kExitOk : dh::kExitError;
    }

    int RunOnce(const std::wstring& command) {
        const auto error = ConnectToSteamVR();
        if (error != vr::VRInitError_None) {
            Print("SteamVR is not running or not reachable: %s", vr::VR_GetVRInitErrorAsEnglishDescription(error));
            return dh::kExitError;
        }

        if (command == L"--toggle") {
            Toggle();
        } else if (command == L"--open") {
            if (!vr::VROverlay()->IsDashboardVisible() || !IsDesktopOverlayActive(OverlayKey())) {
                OpenDesktop();
            }
        } else if (command == L"--close") {
            if (vr::VROverlay()->IsDashboardVisible()) {
                CloseDashboard();
            }
        }

        DisconnectFromSteamVR();
        return dh::kExitOk;
    }

    std::string DeviceString(vr::TrackedDeviceIndex_t index, vr::ETrackedDeviceProperty prop) {
        char buffer[256]{};
        vr::ETrackedPropertyError error = vr::TrackedProp_Success;
        vr::VRSystem()->GetStringTrackedDeviceProperty(index, prop, buffer, sizeof(buffer), &error);
        return error == vr::TrackedProp_Success ? buffer : "";
    }

    void PrintDevices() {
        Print("Tracked devices:");
        for (vr::TrackedDeviceIndex_t i = 0; i < vr::k_unMaxTrackedDeviceCount; i++) {
            const auto deviceClass = vr::VRSystem()->GetTrackedDeviceClass(i);
            if (deviceClass == vr::TrackedDeviceClass_Invalid) {
                continue;
            }
            Print("  [%u] class %d, role %d, type '%s', model '%s'",
                  i,
                  static_cast<int>(deviceClass),
                  static_cast<int>(vr::VRSystem()->GetControllerRoleForTrackedDeviceIndex(i)),
                  DeviceString(i, vr::Prop_ControllerType_String).c_str(),
                  DeviceString(i, vr::Prop_ModelNumber_String).c_str());
        }
    }

    int RunProbe() {
        Print("Build:       %s", dh::kBuildId);
        Print("Config file: %s", dh::Narrow(g_ini).c_str());
        Print("Log file:    %s", dh::Narrow(g_logPath).c_str());

        const std::wstring hotkeyText = dh::IniString(g_ini, L"hotkey", L"keys", kDefaultHotkey);
        UINT modifiers = 0, vk = 0;
        Print("Hotkey:      %s (%s)", dh::Narrow(hotkeyText).c_str(), ParseHotkey(hotkeyText, modifiers, vk) ? "valid" : "INVALID");

        Print("Driver:      %s", IsDriverLoaded() ? "loaded" : "NOT loaded (install it and enable it in SteamVR > Manage Add-ons)");
        PrintEnvironment();

        const auto error = ConnectToSteamVR();
        if (error != vr::VRInitError_None) {
            Print("SteamVR:     not reachable (%s)", vr::VR_GetVRInitErrorAsEnglishDescription(error));
            return dh::kExitError;
        }
        auto* overlay = vr::VROverlay();
        Print("SteamVR:     connected, dashboard %s", overlay->IsDashboardVisible() ? "OPEN" : "closed");
        PrintDevices();

        std::vector<std::string> candidates = {
            OverlayKey(),
            "valve.steam.desktop",
            "system.desktop",
            "system.desktop.1",
            "system.desktop.all",
            "valve.steam.desktop.1",
            "system.systemui",
            "valve.steam.bigpicture",
        };
        Print("Overlay keys (FOUND = exists, ACTIVE = currently shown in the dashboard):");
        std::vector<std::string> seen;
        for (const auto& key : candidates) {
            if (key.empty() || std::find(seen.begin(), seen.end(), key) != seen.end()) {
                continue;
            }
            seen.push_back(key);
            vr::VROverlayHandle_t handle = vr::k_ulOverlayHandleInvalid;
            const bool found = overlay->FindOverlay(key.c_str(), &handle) == vr::VROverlayError_None;
            const bool active = found && overlay->IsActiveDashboardOverlay(handle);
            Print("  %-28s %s%s", key.c_str(), found ? "FOUND" : "-", active ? "  ACTIVE" : "");
        }
        Print("Tip: open the Desktop page in the SteamVR dashboard and run --probe again to see which key is ACTIVE.");

        DisconnectFromSteamVR();
        return dh::kExitOk;
    }

    void PrintUsage() {
        Print("SteamVR-DesktopHotkey helper\n"
              "  --background  run in background (started automatically by the driver)\n"
              "  --toggle      open the desktop view / close the dashboard\n"
              "  --open        open the dashboard on the desktop view\n"
              "  --close       close the dashboard\n"
              "  --press       press the headset input directly (diagnostics)\n"
              "  --probe       print diagnostics");
    }

} // namespace

int WINAPI wWinMain(HINSTANCE, HINSTANCE, LPWSTR, int) {
    InitLogging();
    g_driverRoot = dh::DriverRootFromModule(nullptr);
    g_ini = dh::ConfigPath(g_driverRoot);

    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    const std::wstring command = (argv && argc > 1) ? Lower(argv[1]) : L"";
    if (argv) {
        LocalFree(argv);
    }

    int result = dh::kExitOk;
    if (command == L"--background") {
        result = RunBackground();
    } else if (command == L"--toggle" || command == L"--open" || command == L"--close") {
        result = RunOnce(command);
    } else if (command == L"--press") {
        result = RunPress();
    } else if (command == L"--probe") {
        result = RunProbe();
    } else {
        PrintUsage();
        if (!g_console) {
            MessageBoxW(nullptr,
                        L"This helper is started automatically by the SteamVR driver.\n\n"
                        L"Command line options:\n"
                        L"  --toggle   open desktop view / close dashboard\n"
                        L"  --open     open dashboard on desktop view\n"
                        L"  --close    close dashboard (look away from the panel)\n"
                        L"  --press    press the headset input (diagnostics)\n"
                        L"  --probe    diagnostics (run from a console)",
                        L"SteamVR-DesktopHotkey",
                        MB_OK | MB_ICONINFORMATION);
        }
    }

    if (g_console) {
        FreeConsole();
    }
    return result;
}
