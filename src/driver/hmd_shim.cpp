// SteamVR-DesktopHotkey - headset driver shim
//
// SteamVR only acts on the "open/close dashboard" action when it comes from the headset
// (/user/head), so this shim adds one extra input to whatever headset driver is in use:
//
//   1. IVRServerDriverHost::TrackedDeviceAdded is hooked, and the HMD device passed through it is
//      wrapped by HmdShimDriver. Every call is forwarded to the original driver unchanged.
//   2. After the original driver's Activate() has run, we read the input profile it has just set,
//      generate a copy of it with one input added (/input/desktop_hotkey) plus a matching dashboard
//      binding, and point the headset at the generated profile. All original inputs are kept, so
//      whatever the headset driver provides (volume buttons, taps, proximity, ...) keeps working.
//   3. Pressing that input from the helper's hotkey toggles the SteamVR dashboard.
//
// SPDX-License-Identifier: MIT

#include "hmd_shim.h"

#include "../common/common.h"
#include "log.h"

#include <openvr_driver.h>

#include <MinHook.h>
#include <json.hpp>

#include <atomic>
#include <chrono>
#include <fstream>
#include <string>
#include <thread>

namespace {
    using json = nlohmann::json;

    constexpr const char* kInputPath = "/input/desktop_hotkey";
    constexpr const char* kInputComponent = "/input/desktop_hotkey/click";
    constexpr const char* kBindingSourcePath = "/user/head/input/desktop_hotkey";
    constexpr const char* kCompositorAppKey = "openvr.component.vrcompositor";
    constexpr const char* kGeneratedProfileFile = "generated_hmd_profile.json";
    constexpr const char* kGeneratedBindingsFile = "generated_hmd_bindings_vrcompositor.json";

    std::atomic<vr::VRInputComponentHandle_t> g_component{vr::k_ulInvalidInputComponentHandle};

    // ---------------------------------------------------------------------------------------------
    // Paths and files
    // ---------------------------------------------------------------------------------------------
    std::wstring ThisDriverRootW() {
        HMODULE self = nullptr;
        GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           reinterpret_cast<LPCWSTR>(&ThisDriverRootW),
                           &self);
        return dh::DriverRootFromModule(self);
    }

    std::string ThisDriverRoot() {
        return dh::Narrow(ThisDriverRootW());
    }

    std::string DirectoryOf(const std::string& path) {
        const auto slash = path.find_last_of("\\/");
        return slash == std::string::npos ? std::string(".") : path.substr(0, slash);
    }

    // Turns a SteamVR resource path such as "{mydriver}/input/profile.json" into a real file path.
    std::string ResolveResourcePath(const std::string& resourcePath, const std::string& relativeTo) {
        if (resourcePath.empty()) {
            return {};
        }
        if (resourcePath.front() != '{') {
            // Relative to the file that referenced it, or already a full path.
            if (resourcePath.size() > 1 && (resourcePath[1] == ':' || resourcePath[0] == '\\' || resourcePath[0] == '/')) {
                return resourcePath;
            }
            return relativeTo.empty() ? resourcePath : relativeTo + "\\" + resourcePath;
        }

        char buffer[MAX_PATH * 2]{};
        const uint32_t needed =
            vr::VRResources()->GetResourceFullPath(resourcePath.c_str(), nullptr, buffer, sizeof(buffer));
        if (needed == 0 || needed > sizeof(buffer)) {
            dh::Log("Could not resolve resource path '%s'", resourcePath.c_str());
            return {};
        }
        return buffer;
    }

    bool ReadJsonFile(const std::string& path, json& out) {
        std::ifstream file(path);
        if (!file.is_open()) {
            dh::Log("Cannot open '%s'", path.c_str());
            return false;
        }
        try {
            file >> out;
        } catch (const std::exception& e) {
            dh::Log("Cannot parse '%s': %s", path.c_str(), e.what());
            return false;
        }
        return true;
    }

    bool WriteJsonFile(const std::string& path, const json& value) {
        std::ofstream file(path, std::ios::trunc);
        if (!file.is_open()) {
            dh::Log("Cannot write '%s'", path.c_str());
            return false;
        }
        file << value.dump(2);
        return file.good();
    }

    std::string GeneratedDirectory() {
        const std::string directory = ThisDriverRoot() + "\\resources\\input";
        CreateDirectoryW(dh::Widen(ThisDriverRoot() + "\\resources").c_str(), nullptr);
        CreateDirectoryW(dh::Widen(directory).c_str(), nullptr);
        return directory;
    }

    // ---------------------------------------------------------------------------------------------
    // Profile generation
    // ---------------------------------------------------------------------------------------------
    json MakeOurBindingSource() {
        json source;
        source["inputs"]["click"]["output"] = "/actions/system/in/opendashboard";
        source["mode"] = "button";
        source["path"] = kBindingSourcePath;
        return source;
    }

    // Copies the dashboard binding file of the original profile and appends our source to it.
    // Returns the file name of the generated binding file (relative to the generated profile).
    std::string GenerateBindings(const json& originalProfile, const std::string& originalProfileDirectory) {
        json bindings;
        bool haveOriginal = false;

        if (originalProfile.contains("default_bindings") && originalProfile["default_bindings"].is_array()) {
            for (const auto& entry : originalProfile["default_bindings"]) {
                if (!entry.contains("app_key") || entry["app_key"] != kCompositorAppKey) {
                    continue;
                }
                const std::string url = entry.value("binding_url", "");
                const std::string full = ResolveResourcePath(url, originalProfileDirectory);
                if (!full.empty() && ReadJsonFile(full, bindings)) {
                    haveOriginal = true;
                    dh::Log("Using headset dashboard bindings from '%s'", full.c_str());
                }
                break;
            }
        }

        if (!haveOriginal) {
            dh::Log("The headset has no dashboard bindings of its own, generating a fresh file");
            bindings = json::object();
            bindings["action_manifest_version"] = 0;
            bindings["alias_info"] = json::object();
            bindings["app_key"] = kCompositorAppKey;
            bindings["bindings"] = json::object();
            bindings["category"] = "steamvr_input";
            bindings["options"] = json::object();
            bindings["simulated_actions"] = json::array();
            if (originalProfile.contains("controller_type")) {
                bindings["controller_type"] = originalProfile["controller_type"];
            }
        }

        bindings["description"] = "Generated by SteamVR-DesktopHotkey";
        bindings["name"] = "SteamVR Dashboard bindings with the Desktop Hotkey input";

        if (!bindings.contains("bindings") || !bindings["bindings"].is_object()) {
            bindings["bindings"] = json::object();
        }
        json& system = bindings["bindings"]["/actions/system"];
        if (!system.is_object()) {
            system = json::object();
        }
        if (!system.contains("sources") || !system["sources"].is_array()) {
            system["sources"] = json::array();
        }
        // Drop a source we may have added to this file before, then append a fresh one.
        json cleaned = json::array();
        for (const auto& source : system["sources"]) {
            if (source.value("path", "") != kBindingSourcePath) {
                cleaned.push_back(source);
            }
        }
        cleaned.push_back(MakeOurBindingSource());
        system["sources"] = cleaned;

        const std::string outputPath = GeneratedDirectory() + "\\" + kGeneratedBindingsFile;
        if (!WriteJsonFile(outputPath, bindings)) {
            return {};
        }
        dh::Log("Wrote '%s'", outputPath.c_str());
        return kGeneratedBindingsFile;
    }

    // Generates a copy of the headset's input profile with our extra input, and returns the resource
    // path of the generated profile (empty on failure).
    std::string GenerateProfile(const std::string& originalProfileResource) {
        const std::string originalPath = ResolveResourcePath(originalProfileResource, {});
        json profile;
        std::string originalDirectory;

        if (originalPath.empty() || !ReadJsonFile(originalPath, profile)) {
            dh::Log("The headset has no readable input profile, generating a minimal one");
            profile = json::object();
            profile["jsonid"] = "input_profile";
            profile["controller_type"] = "desktop_hotkey_hmd";
            profile["input_bindingui_mode"] = "hmd";
            profile["input_source"] = json::object();
        } else {
            originalDirectory = DirectoryOf(originalPath);
            dh::Log("Extending the headset input profile '%s'", originalPath.c_str());
        }

        if (!profile.contains("input_source") || !profile["input_source"].is_object()) {
            profile["input_source"] = json::object();
        }
        json& input = profile["input_source"][kInputPath];
        input = json::object();
        input["type"] = "button";
        input["click"] = true;
        input["order"] = 99;
        input["binding_image_point"] = json::array({132, 75});

        const std::string bindingFile = GenerateBindings(profile, originalDirectory);
        if (bindingFile.empty()) {
            return {};
        }

        // Point the generated profile at the generated bindings, keeping bindings for other apps.
        json defaults = json::array();
        if (profile.contains("default_bindings") && profile["default_bindings"].is_array()) {
            for (const auto& entry : profile["default_bindings"]) {
                if (entry.value("app_key", "") != kCompositorAppKey) {
                    defaults.push_back(entry);
                }
            }
        }
        json compositorEntry;
        compositorEntry["app_key"] = kCompositorAppKey;
        compositorEntry["binding_url"] = bindingFile;
        defaults.push_back(compositorEntry);
        profile["default_bindings"] = defaults;

        const std::string outputPath = GeneratedDirectory() + "\\" + kGeneratedProfileFile;
        if (!WriteJsonFile(outputPath, profile)) {
            return {};
        }
        dh::Log("Wrote '%s'", outputPath.c_str());
        return std::string("{") + dh::kDriverName + "}/input/" + kGeneratedProfileFile;
    }

    // ---------------------------------------------------------------------------------------------
    // The shim device: forwards everything, adds one input
    // ---------------------------------------------------------------------------------------------
    class HmdShimDriver final : public vr::ITrackedDeviceServerDriver {
      public:
        explicit HmdShimDriver(vr::ITrackedDeviceServerDriver* inner) : m_inner(inner) {
        }

        vr::EVRInitError Activate(uint32_t objectId) override {
            const auto status = m_inner->Activate(objectId);
            if (status != vr::VRInitError_None) {
                dh::Log("The headset driver failed to activate (%d), not adding our input", static_cast<int>(status));
                return status;
            }

            const auto container = vr::VRProperties()->TrackedDeviceToPropertyContainer(objectId);
            const std::string originalProfile =
                vr::VRProperties()->GetStringProperty(container, vr::Prop_InputProfilePath_String);
            dh::Log("Headset activated (id %u), its input profile is '%s'", objectId, originalProfile.c_str());

            const std::string generated = GenerateProfile(originalProfile);
            if (generated.empty()) {
                dh::Log("Could not generate the extended input profile, leaving the headset untouched");
                return status;
            }

            vr::VRInputComponentHandle_t component = vr::k_ulInvalidInputComponentHandle;
            const auto inputError = vr::VRDriverInput()->CreateBooleanComponent(container, kInputComponent, &component);
            if (inputError != vr::VRInputError_None) {
                dh::Log("CreateBooleanComponent('%s') failed: %d", kInputComponent, static_cast<int>(inputError));
                return status;
            }

            vr::VRProperties()->SetStringProperty(container, vr::Prop_InputProfilePath_String, generated.c_str());
            g_component = component;
            dh::Log("Added '%s' to the headset and switched it to '%s'", kInputComponent, generated.c_str());
            return status;
        }

        void Deactivate() override {
            g_component = vr::k_ulInvalidInputComponentHandle;
            m_inner->Deactivate();
        }

        void EnterStandby() override {
            m_inner->EnterStandby();
        }

        void* GetComponent(const char* componentNameAndVersion) override {
            return m_inner->GetComponent(componentNameAndVersion);
        }

        void DebugRequest(const char* request, char* responseBuffer, uint32_t responseBufferSize) override {
            m_inner->DebugRequest(request, responseBuffer, responseBufferSize);
        }

        vr::DriverPose_t GetPose() override {
            return m_inner->GetPose();
        }

      private:
        vr::ITrackedDeviceServerDriver* const m_inner;
    };

    // ---------------------------------------------------------------------------------------------
    // The hook
    // ---------------------------------------------------------------------------------------------
    // x64 has a single calling convention, so the implicit "this" is just the first argument.
    using TrackedDeviceAddedFn =
        bool (*)(vr::IVRServerDriverHost*, const char*, vr::ETrackedDeviceClass, vr::ITrackedDeviceServerDriver*);
    TrackedDeviceAddedFn g_originalTrackedDeviceAdded = nullptr;

    bool HookedTrackedDeviceAdded(vr::IVRServerDriverHost* host,
                                             const char* serialNumber,
                                             vr::ETrackedDeviceClass deviceClass,
                                             vr::ITrackedDeviceServerDriver* driver) {
        vr::ITrackedDeviceServerDriver* passedDriver = driver;
        if (deviceClass == vr::TrackedDeviceClass_HMD && g_component == vr::k_ulInvalidInputComponentHandle) {
            dh::Log("Shimming headset '%s'", serialNumber ? serialNumber : "?");
            passedDriver = new HmdShimDriver(driver);
        }
        return g_originalTrackedDeviceAdded(host, serialNumber, deviceClass, passedDriver);
    }

} // namespace

namespace dh {

    bool InstallHmdShim() {
        if (MH_Initialize() != MH_OK) {
            Log("MinHook could not be initialised");
            return false;
        }

        vr::EVRInitError error = vr::VRInitError_None;
        void* host = vr::VRDriverContext()->GetGenericInterface("IVRServerDriverHost_006", &error);
        if (!host || error != vr::VRInitError_None) {
            Log("IVRServerDriverHost_006 is not available (%d)", static_cast<int>(error));
            return false;
        }

        // TrackedDeviceAdded is the first method of the interface.
        void** vtable = *reinterpret_cast<void***>(host);
        void* target = vtable[0];

        if (MH_CreateHook(target,
                          reinterpret_cast<void*>(&HookedTrackedDeviceAdded),
                          reinterpret_cast<void**>(&g_originalTrackedDeviceAdded)) != MH_OK ||
            MH_EnableHook(target) != MH_OK) {
            Log("Could not hook IVRServerDriverHost::TrackedDeviceAdded");
            return false;
        }

        Log("Headset shim installed");
        return true;
    }

    bool IsHmdShimReady() {
        return g_component != vr::k_ulInvalidInputComponentHandle;
    }

    bool PressHotkeyInput(int pressDurationMs) {
        const vr::VRInputComponentHandle_t component = g_component;
        if (component == vr::k_ulInvalidInputComponentHandle) {
            Log("The headset input is not ready yet, ignoring the press");
            return false;
        }

        Log("Pressing the headset hotkey input for %d ms", pressDurationMs);
        vr::VRDriverInput()->UpdateBooleanComponent(component, true, 0.0);
        std::this_thread::sleep_for(std::chrono::milliseconds(pressDurationMs));
        vr::VRDriverInput()->UpdateBooleanComponent(component, false, 0.0);
        return true;
    }

} // namespace dh
