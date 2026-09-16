// SteamVR-DesktopHotkey
// Shared definitions between the SteamVR driver and the helper process.
// SPDX-License-Identifier: MIT
#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <string>

namespace dh {

    // Driver name as declared in driver.vrdrivermanifest.
    inline constexpr const char* kDriverName = "desktop_hotkey";

    // Named kernel objects used for driver <-> helper communication.
    // "Local\" = current Windows session (vrserver and the helper run in the same one).
    inline constexpr const wchar_t* kPressEventName = L"Local\\SteamVR-DesktopHotkey.Press";
    inline constexpr const wchar_t* kQuitEventName = L"Local\\SteamVR-DesktopHotkey.Quit";
    inline constexpr const wchar_t* kHelperMutexName = L"Local\\SteamVR-DesktopHotkey.Helper";

    // Helper exit codes. The driver restarts the helper only on kExitRetry.
    enum ExitCode : int {
        kExitOk = 0,
        kExitError = 1,
        kExitRetry = 2,
    };

    // Directory containing the given module (dll or exe), without trailing slash.
    inline std::wstring ModuleDirectory(HMODULE module) {
        std::wstring path(MAX_PATH, L'\0');
        for (;;) {
            DWORD len = GetModuleFileNameW(module, path.data(), static_cast<DWORD>(path.size()));
            if (len == 0) {
                return L".";
            }
            if (len < path.size()) {
                path.resize(len);
                break;
            }
            path.resize(path.size() * 2);
        }
        const auto slash = path.find_last_of(L"\\/");
        return slash == std::wstring::npos ? L"." : path.substr(0, slash);
    }

    // Both binaries live in <driver root>\bin\win64\ -> go up two levels.
    inline std::wstring DriverRootFromModule(HMODULE module) {
        std::wstring dir = ModuleDirectory(module);
        for (int i = 0; i < 2; i++) {
            const auto slash = dir.find_last_of(L"\\/");
            if (slash == std::wstring::npos) {
                break;
            }
            dir.resize(slash);
        }
        return dir;
    }

    inline std::wstring ConfigPath(const std::wstring& driverRoot) {
        return driverRoot + L"\\config.ini";
    }

    inline std::wstring Trim(const std::wstring& s) {
        const auto first = s.find_first_not_of(L" \t\r\n\"");
        if (first == std::wstring::npos) {
            return {};
        }
        const auto last = s.find_last_not_of(L" \t\r\n\"");
        return s.substr(first, last - first + 1);
    }

    inline std::wstring IniString(const std::wstring& ini,
                                  const wchar_t* section,
                                  const wchar_t* key,
                                  const wchar_t* fallback) {
        wchar_t buffer[1024]{};
        GetPrivateProfileStringW(section, key, fallback, buffer, ARRAYSIZE(buffer), ini.c_str());
        return Trim(buffer);
    }

    inline int IniInt(const std::wstring& ini, const wchar_t* section, const wchar_t* key, int fallback) {
        return static_cast<int>(GetPrivateProfileIntW(section, key, fallback, ini.c_str()));
    }

    inline std::string Narrow(const std::wstring& s) {
        if (s.empty()) {
            return {};
        }
        int len = WideCharToMultiByte(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0, nullptr, nullptr);
        std::string out(static_cast<size_t>(len), '\0');
        WideCharToMultiByte(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), out.data(), len, nullptr, nullptr);
        return out;
    }

} // namespace dh
