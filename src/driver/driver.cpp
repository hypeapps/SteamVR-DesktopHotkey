// SteamVR-DesktopHotkey - SteamVR driver
//
// A tiny, standalone SteamVR driver. It does NOT hook anything and does NOT touch the
// headset driver. It only:
//   1. registers one virtual, never-tracked device with a single "system" button,
//      whose default binding in the SteamVR dashboard (vrcompositor) is "open/close dashboard";
//   2. presses that button whenever the helper process signals a named event;
//   3. starts (and, if it crashes, restarts) the helper process that listens for the hotkey.
//
// SPDX-License-Identifier: MIT

#include "../common/common.h"

#include <openvr_driver.h>

#include <atomic>
#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <memory>
#include <thread>

namespace {
    using namespace std::chrono_literals;
    using Clock = std::chrono::steady_clock;

    void Log(const char* format, ...) {
        char message[1024];
        va_list args;
        va_start(args, format);
        vsnprintf(message, sizeof(message), format, args);
        va_end(args);

        if (vr::VRDriverLog()) {
            char line[1100];
            snprintf(line, sizeof(line), "[desktop_hotkey] %s\n", message);
            vr::VRDriverLog()->Log(line);
        }
    }

    std::wstring ThisDriverRoot() {
        HMODULE self = nullptr;
        GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           reinterpret_cast<LPCWSTR>(&ThisDriverRoot),
                           &self);
        return dh::DriverRootFromModule(self);
    }

    vr::ETrackedControllerRole ParseRole(const std::wstring& value) {
        if (_wcsicmp(value.c_str(), L"stylus") == 0) {
            return vr::TrackedControllerRole_Stylus;
        }
        if (_wcsicmp(value.c_str(), L"left") == 0) {
            return vr::TrackedControllerRole_LeftHand;
        }
        if (_wcsicmp(value.c_str(), L"right") == 0) {
            return vr::TrackedControllerRole_RightHand;
        }
        if (_wcsicmp(value.c_str(), L"opt_out") == 0) {
            return vr::TrackedControllerRole_OptOut;
        }
        return vr::TrackedControllerRole_Treadmill;
    }

    // ---------------------------------------------------------------------------------------------
    // The virtual button device.
    // ---------------------------------------------------------------------------------------------
    class ButtonDevice final : public vr::ITrackedDeviceServerDriver {
      public:
        ButtonDevice(vr::ETrackedControllerRole role, bool reportPose) : m_role(role), m_reportPose(reportPose) {
        }

        vr::EVRInitError Activate(uint32_t objectId) override {
            m_objectId = objectId;

            auto* props = vr::VRProperties();
            const auto container = props->TrackedDeviceToPropertyContainer(objectId);

            props->SetStringProperty(container, vr::Prop_ModelNumber_String, "Desktop Hotkey");
            props->SetStringProperty(container, vr::Prop_ManufacturerName_String, "SteamVR-DesktopHotkey");
            props->SetStringProperty(container, vr::Prop_TrackingSystemName_String, dh::kDriverName);
            props->SetStringProperty(container, vr::Prop_ControllerType_String, "desktop_hotkey");
            props->SetStringProperty(
                container, vr::Prop_InputProfilePath_String, "{desktop_hotkey}/input/desktop_hotkey_profile.json");

            // Not a hand controller: never let SteamVR or games pick it as a left/right hand.
            props->SetInt32Property(container, vr::Prop_ControllerRoleHint_Int32, m_role);
            props->SetInt32Property(container, vr::Prop_ControllerHandSelectionPriority_Int32, -1000000);

            // No render model, no battery, nothing to power off.
            props->SetStringProperty(container, vr::Prop_RenderModelName_String, "");
            props->SetBoolProperty(container, vr::Prop_NeverTracked_Bool, !m_reportPose);
            props->SetBoolProperty(container, vr::Prop_DeviceProvidesBatteryStatus_Bool, false);
            props->SetBoolProperty(container, vr::Prop_DeviceCanPowerOff_Bool, false);
            props->SetBoolProperty(container, vr::Prop_Identifiable_Bool, false);

            const auto err = vr::VRDriverInput()->CreateBooleanComponent(container, "/input/system/click", &m_click);
            if (err != vr::VRInputError_None) {
                Log("CreateBooleanComponent(/input/system/click) failed: %d", static_cast<int>(err));
            }

            vr::VRServerDriverHost()->TrackedDevicePoseUpdated(objectId, GetPose(), sizeof(vr::DriverPose_t));

            m_active = true;
            Log("Virtual button device activated (id %u, role %d)", objectId, static_cast<int>(m_role));
            return vr::VRInitError_None;
        }

        void Deactivate() override {
            m_active = false;
            m_objectId = vr::k_unTrackedDeviceIndexInvalid;
        }

        void EnterStandby() override {
        }

        void* GetComponent(const char*) override {
            return nullptr;
        }

        void DebugRequest(const char*, char* response, uint32_t responseSize) override {
            if (responseSize > 0) {
                response[0] = '\0';
            }
        }

        vr::DriverPose_t GetPose() override {
            vr::DriverPose_t pose{};
            pose.qWorldFromDriverRotation.w = 1.0;
            pose.qDriverFromHeadRotation.w = 1.0;
            pose.qRotation.w = 1.0;
            pose.deviceIsConnected = true;
            // Some SteamVR versions ignore input from devices that never report a pose, so by default
            // we report a static identity pose. The device still has no render model and is not drawn.
            pose.poseIsValid = m_reportPose;
            pose.result = m_reportPose ? vr::TrackingResult_Running_OK : vr::TrackingResult_Uninitialized;
            return pose;
        }

        void RefreshPose() {
            if (m_active && m_objectId != vr::k_unTrackedDeviceIndexInvalid) {
                vr::VRServerDriverHost()->TrackedDevicePoseUpdated(m_objectId, GetPose(), sizeof(vr::DriverPose_t));
            }
        }

        void SetPressed(bool pressed) {
            if (m_active && m_click != vr::k_ulInvalidInputComponentHandle) {
                vr::VRDriverInput()->UpdateBooleanComponent(m_click, pressed, 0.0);
            }
        }

      private:
        const vr::ETrackedControllerRole m_role;
        const bool m_reportPose;
        std::atomic<bool> m_active{false};
        uint32_t m_objectId = vr::k_unTrackedDeviceIndexInvalid;
        vr::VRInputComponentHandle_t m_click = vr::k_ulInvalidInputComponentHandle;
    };

    // ---------------------------------------------------------------------------------------------
    // The device provider (driver entry point object).
    // ---------------------------------------------------------------------------------------------
    class Provider final : public vr::IServerTrackedDeviceProvider {
      public:
        vr::EVRInitError Init(vr::IVRDriverContext* context) override {
            VR_INIT_SERVER_DRIVER_CONTEXT(context);

            m_root = ThisDriverRoot();
            m_ini = dh::ConfigPath(m_root);
            Log("Driver root: %s", dh::Narrow(m_root).c_str());

            const auto role = ParseRole(dh::IniString(m_ini, L"driver", L"role", L"treadmill"));
            m_pressDurationMs = dh::IniInt(m_ini, L"driver", L"press_duration_ms", 80);
            if (m_pressDurationMs < 20 || m_pressDurationMs > 500) {
                m_pressDurationMs = 80;
            }

            m_pressEvent = CreateEventW(nullptr, FALSE /* auto-reset */, FALSE, dh::kPressEventName);
            m_quitEvent = CreateEventW(nullptr, TRUE /* manual reset */, FALSE, dh::kQuitEventName);
            if (m_quitEvent) {
                ResetEvent(m_quitEvent); // may be left signaled by a previous session
            }
            if (!m_pressEvent) {
                Log("CreateEvent(press) failed: %lu", GetLastError());
            }

            const bool reportPose = dh::IniInt(m_ini, L"driver", L"report_pose", 1) != 0;
            m_device = std::make_unique<ButtonDevice>(role, reportPose);
            if (!vr::VRServerDriverHost()->TrackedDeviceAdded(
                    "desktop_hotkey_button", vr::TrackedDeviceClass_Controller, m_device.get())) {
                Log("TrackedDeviceAdded failed");
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
            auto nextPoseUpdate = Clock::now() + 1s;

            while (m_running) {
                const DWORD wait = m_pressEvent ? WaitForSingleObject(m_pressEvent, 50) : (Sleep(50), WAIT_TIMEOUT);
                if (!m_running) {
                    break;
                }

                if (wait == WAIT_OBJECT_0) {
                    Log("Press requested, holding the virtual system button for %d ms", m_pressDurationMs);
                    m_device->SetPressed(true);
                    std::this_thread::sleep_for(std::chrono::milliseconds(m_pressDurationMs));
                    m_device->SetPressed(false);
                    Log("Virtual system button released");
                }

                if (Clock::now() >= nextPoseUpdate) {
                    nextPoseUpdate = Clock::now() + 1s;
                    m_device->RefreshPose();
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
        int m_pressDurationMs = 80;

        std::unique_ptr<ButtonDevice> m_device;
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
