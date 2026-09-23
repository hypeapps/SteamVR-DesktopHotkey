// SteamVR-DesktopHotkey - SteamVR driver
//
// The driver itself does very little:
//   1. it installs the headset shim (see hmd_shim.cpp), which adds one extra input to the headset
//      that is bound to "open/close dashboard";
//   2. it waits for the helper process to signal a named event and then presses that input;
//   3. it starts the helper together with SteamVR and restarts it if it crashes.
//
// SPDX-License-Identifier: MIT

#include "../common/common.h"
#include "hmd_shim.h"
#include "log.h"

#include <openvr_driver.h>

#include <atomic>
#include <chrono>
#include <cstring>
#include <thread>

namespace {
    using namespace std::chrono_literals;
    using Clock = std::chrono::steady_clock;
    using dh::Log;

    std::wstring ThisDriverRoot() {
        HMODULE self = nullptr;
        GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           reinterpret_cast<LPCWSTR>(&ThisDriverRoot),
                           &self);
        return dh::DriverRootFromModule(self);
    }

    class Provider final : public vr::IServerTrackedDeviceProvider {
      public:
        vr::EVRInitError Init(vr::IVRDriverContext* context) override {
            VR_INIT_SERVER_DRIVER_CONTEXT(context);

            m_root = ThisDriverRoot();
            m_ini = dh::ConfigPath(m_root);
            Log("Driver root: %s", dh::Narrow(m_root).c_str());

            m_useSystemButton = dh::IniInt(m_ini, L"driver", L"use_system_button", 1) != 0;
            m_pressDurationMs = dh::IniInt(m_ini, L"driver", L"press_duration_ms", 120);
            if (m_pressDurationMs < 20 || m_pressDurationMs > 1000) {
                m_pressDurationMs = 120;
            }

            m_pressEvent = CreateEventW(nullptr, FALSE /* auto-reset */, FALSE, dh::kPressEventName);
            m_quitEvent = CreateEventW(nullptr, TRUE /* manual reset */, FALSE, dh::kQuitEventName);
            if (m_quitEvent) {
                ResetEvent(m_quitEvent); // may be left signaled by a previous session
            }
            if (!m_pressEvent) {
                Log("CreateEvent(press) failed: %lu", GetLastError());
            }

            if (!dh::InstallHmdShim()) {
                Log("The headset shim could not be installed, the hotkey will not be able to close the dashboard");
            }

            m_running = true;
            m_worker = std::thread(&Provider::WorkerThread, this);
            return vr::VRInitError_None;
        }

        void Cleanup() override {
            m_running = false;
            if (m_quitEvent) {
                SetEvent(m_quitEvent); // ask the helper to exit
            }
            if (m_worker.joinable()) {
                m_worker.join();
            }
            if (m_helperProcess) {
                CloseHandle(m_helperProcess);
                m_helperProcess = nullptr;
            }
            if (m_pressEvent) {
                CloseHandle(m_pressEvent);
                m_pressEvent = nullptr;
            }
            if (m_quitEvent) {
                CloseHandle(m_quitEvent);
                m_quitEvent = nullptr;
            }
            VR_CLEANUP_SERVER_DRIVER_CONTEXT();
        }

        const char* const* GetInterfaceVersions() override {
            return vr::k_InterfaceVersions;
        }

        void RunFrame() override {
            vr::VREvent_t event{};
            while (vr::VRServerDriverHost()->PollNextEvent(&event, sizeof(event))) {
            }
        }

        bool ShouldBlockStandbyMode() override {
            return false;
        }

        void EnterStandby() override {
        }

        void LeaveStandby() override {
        }

      private:
        void WorkerThread() {
            // Give vrserver a moment to finish starting before launching the helper.
            auto nextHelperCheck = Clock::now() + 2s;
            const auto shimDeadline = Clock::now() + 20s;
            auto nextProfileCheck = Clock::now() + 2s;
            bool warnedAboutShim = false;

            while (m_running) {
                const DWORD wait = m_pressEvent ? WaitForSingleObject(m_pressEvent, 50) : (Sleep(50), WAIT_TIMEOUT);
                if (!m_running) {
                    break;
                }

                if (wait == WAIT_OBJECT_0) {
                    dh::PressHotkeyInput(m_pressDurationMs, m_useSystemButton);
                }

                if (Clock::now() >= nextProfileCheck) {
                    nextProfileCheck = Clock::now() + 2s;
                    dh::MaintainHmdProfile();
                }

                if (!warnedAboutShim && Clock::now() >= shimDeadline && !dh::IsHmdShimReady()) {
                    warnedAboutShim = true;
                    Log("No headset went through our shim. This driver most likely loaded after the headset "
                        "driver - check that resources/settings/default.vrsettings still sets a high loadPriority.");
                }

                if (Clock::now() >= nextHelperCheck) {
                    nextHelperCheck = Clock::now() + 3s;
                    SuperviseHelper();
                }
            }
        }

        void SuperviseHelper() {
            if (m_helperGaveUp || dh::IniInt(m_ini, L"driver", L"start_helper", 1) == 0) {
                return;
            }

            if (m_helperProcess) {
                DWORD code = 0;
                if (GetExitCodeProcess(m_helperProcess, &code) && code == STILL_ACTIVE) {
                    return;
                }
                CloseHandle(m_helperProcess);
                m_helperProcess = nullptr;

                if (code != dh::kExitRetry) {
                    Log("Helper exited with code %lu, not restarting", code);
                    m_helperGaveUp = true;
                    return;
                }
                if (++m_helperRestarts > 5) {
                    Log("Helper keeps failing, giving up");
                    m_helperGaveUp = true;
                    return;
                }
                Log("Restarting helper (attempt %d)", m_helperRestarts);
            }

            LaunchHelper();
        }

        void LaunchHelper() {
            const std::wstring exe = m_root + L"\\bin\\win64\\desktop_hotkey_helper.exe";
            std::wstring commandLine = L"\"" + exe + L"\" --background";

            STARTUPINFOW startup{};
            startup.cb = sizeof(startup);
            PROCESS_INFORMATION process{};
            if (!CreateProcessW(exe.c_str(),
                                commandLine.data(),
                                nullptr,
                                nullptr,
                                FALSE,
                                CREATE_NO_WINDOW,
                                nullptr,
                                m_root.c_str(),
                                &startup,
                                &process)) {
                Log("Failed to start helper '%s': %lu", dh::Narrow(exe).c_str(), GetLastError());
                m_helperGaveUp = true;
                return;
            }
            CloseHandle(process.hThread);
            m_helperProcess = process.hProcess;
            Log("Helper started (pid %lu)", process.dwProcessId);
        }

        std::wstring m_root;
        std::wstring m_ini;
        int m_pressDurationMs = 120;
        bool m_useSystemButton = true;

        HANDLE m_pressEvent = nullptr;
        HANDLE m_quitEvent = nullptr;

        std::atomic<bool> m_running{false};
        std::thread m_worker;

        HANDLE m_helperProcess = nullptr;
        int m_helperRestarts = 0;
        bool m_helperGaveUp = false;
    };

    Provider g_provider;

} // namespace

extern "C" __declspec(dllexport) void* HmdDriverFactory(const char* interfaceName, int* returnCode) {
    if (std::strcmp(interfaceName, vr::IServerTrackedDeviceProvider_Version) == 0) {
        return &g_provider;
    }
    if (returnCode) {
        *returnCode = vr::VRInitError_Init_InterfaceNotFound;
    }
    return nullptr;
}
