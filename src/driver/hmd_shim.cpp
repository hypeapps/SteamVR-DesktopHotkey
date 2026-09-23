// SteamVR-DesktopHotkey - headset driver shim
//
// SteamVR only lets the headset (/user/head) drive the dashboard, so this shim adds one extra
// input to whatever headset driver is in use:
//
//   1. IVRServerDriverHost::TrackedDeviceAdded is hooked, and the HMD device passed through it is
//      wrapped by HmdShimDriver. Every call is forwarded to the original driver unchanged.
//   2. After the original driver's Activate() has run, we take its input profile (or the one named
//      by base_profile in config.ini), write a copy of it with one input added
//      (/input/desktop_hotkey) and matching dashboard bindings, and point the headset at the copy.
//      The copy gets its own controller type so that SteamVR loads our bindings. All original
//      inputs are kept, so whatever the headset driver provides keeps working.
//   3. IVRProperties::WritePropertyBatch is hooked so that a headset driver setting its own
//      profile a moment later (CustomHeadsetOpenVR does) cannot replace ours.
//
// The extra input is bound the same way SteamVR-Dashboard-KeyboardNav binds its button:
//   - /actions/system/in/opendashboard   opens the dashboard (this action never closes it),
//   - /actions/lasermouse/in/leftclick   a head-aimed click: on empty space next to the dashboard
//                                        panel it closes the dashboard, on the panel it clicks.
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
#include <mutex>
#include <string>
#include <thread>

namespace {
    using json = nlohmann::json;

    constexpr const char* kInputPath = "/input/desktop_hotkey";
    constexpr const char* kInputComponent = "/input/desktop_hotkey/click";
    constexpr const char* kBindingSourcePath = "/user/head/input/desktop_hotkey";
    constexpr const char* kCompositorAppKey = "openvr.component.vrcompositor";
    // Every regeneration writes new file names: SteamVR reloads bindings when the profile path
    // changes, so rewriting the same file in place would leave the old bindings active.
    std::atomic<int> g_generation{0};

    std::string GeneratedProfileFile(int generation) {
        return "generated_hmd_profile_" + std::to_string(generation) + ".json";
    }

    std::string GeneratedBindingsFile(int generation) {
        return "generated_hmd_bindings_vrcompositor_" + std::to_string(generation) + ".json";
    }

    std::atomic<vr::VRInputComponentHandle_t> g_component{vr::k_ulInvalidInputComponentHandle};
    std::atomic<vr::PropertyContainerHandle_t> g_container{vr::k_ulInvalidPropertyContainer};
    std::mutex g_profileMutex;
    std::string g_generatedProfileResource;
    std::string g_generatedControllerType;
    // Buffers handed to SteamVR when we substitute property writes; they must stay alive.
    std::string g_ownedProfileBuffer;
    std::string g_ownedTypeBuffer;
    thread_local bool g_inOwnWrite = false;

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

    std::wstring ConfigFile() {
        return dh::ConfigPath(ThisDriverRootW());
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

    // Deletes the files left behind by previous SteamVR sessions.
    void RemoveGeneratedFiles() {
        const std::wstring pattern = dh::Widen(GeneratedDirectory() + "\\generated_hmd_*.json");
        WIN32_FIND_DATAW found{};
        HANDLE search = FindFirstFileW(pattern.c_str(), &found);
        if (search == INVALID_HANDLE_VALUE) {
            return;
        }
        const std::wstring directory = dh::Widen(GeneratedDirectory());
        do {
            DeleteFileW((directory + L"\\" + found.cFileName).c_str());
        } while (FindNextFileW(search, &found));
        FindClose(search);
    }

    // ---------------------------------------------------------------------------------------------
    // Profile generation
    // ---------------------------------------------------------------------------------------------
    json MakeOurBindingSource(const char* output) {
        json source;
        source["inputs"]["click"]["output"] = output;
        source["mode"] = "button";
        source["path"] = kBindingSourcePath;
        return source;
    }

    // Adds our input as a source of one action set, replacing an entry we may have added before.
    void AddOurSource(json& bindings, const char* actionSet, const char* output) {
        json& set = bindings["bindings"][actionSet];
        if (!set.is_object()) {
            set = json::object();
        }
        if (!set.contains("sources") || !set["sources"].is_array()) {
            set["sources"] = json::array();
        }
        json cleaned = json::array();
        for (const auto& source : set["sources"]) {
            if (source.value("path", "") != kBindingSourcePath) {
                cleaned.push_back(source);
            }
        }
        cleaned.push_back(MakeOurBindingSource(output));
        set["sources"] = cleaned;
    }

    // Copies the dashboard binding file of the original profile and appends our source to it.
    // Returns the file name of the generated binding file (relative to the generated profile).
    std::string GenerateBindings(const json& originalProfile,
                                 const std::string& originalProfileDirectory,
                                 int generation) {
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

        // The binding file must name the same controller type as the generated profile, otherwise
        // it describes a different device than the one it is loaded for.
        if (originalProfile.contains("controller_type")) {
            bindings["controller_type"] = originalProfile["controller_type"];
        }
        bindings["description"] = "Generated by SteamVR-DesktopHotkey";
        bindings["name"] = "SteamVR Dashboard bindings with the Desktop Hotkey input";

        if (!bindings.contains("bindings") || !bindings["bindings"].is_object()) {
            bindings["bindings"] = json::object();
        }
        // Our input is bound the same way SteamVR-Dashboard-KeyboardNav binds its button:
        //  - "opendashboard" opens the dashboard (this action never closes it),
        //  - the head-aimed laser mouse click closes the dashboard when the gaze is on empty space
        //    next to the panel, and clicks whatever the gaze points at on the panel.
        AddOurSource(bindings, "/actions/system", "/actions/system/in/opendashboard");
        AddOurSource(bindings, "/actions/lasermouse", "/actions/lasermouse/in/leftclick");

        // The laser mouse needs a pointer pose; keep the headset's own if the profile had one.
        json& laser = bindings["bindings"]["/actions/lasermouse"];
        if (!laser.contains("poses") || !laser["poses"].is_array() || laser["poses"].empty()) {
            json pose;
            pose["output"] = "/actions/lasermouse/in/pointer";
            pose["path"] = "/user/head/pose/raw";
            laser["poses"] = json::array({pose});
        }

        const std::string fileName = GeneratedBindingsFile(generation);
        const std::string outputPath = GeneratedDirectory() + "\\" + fileName;
        if (!WriteJsonFile(outputPath, bindings)) {
            return {};
        }
        dh::Log("Wrote '%s'", outputPath.c_str());
        return fileName;
    }

    struct GeneratedProfile {
        std::string resource;       // "{desktop_hotkey}/input/generated_hmd_profile_N.json"
        std::string controllerType; // the new controller type the headset has to report

        bool ok() const {
            return !resource.empty();
        }
    };

    // Generates a copy of the headset's input profile with our extra input. The copy gets its own
    // controller type: SteamVR keys loaded bindings by controller type and would otherwise keep the
    // bindings it had already loaded for the original type and never read our file.
    GeneratedProfile GenerateProfile(const std::string& originalProfileResource) {
        const int generation = ++g_generation;
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

        std::string controllerType = profile.value("controller_type", std::string("hmd"));
        if (controllerType.size() < 3 || controllerType.substr(controllerType.size() - 3) != "_dh") {
            controllerType += "_dh";
        }
        profile["controller_type"] = controllerType;

        if (!profile.contains("input_source") || !profile["input_source"].is_object()) {
            profile["input_source"] = json::object();
        }
        json& input = profile["input_source"][kInputPath];
        input = json::object();
        input["type"] = "button";
        input["click"] = true;
        input["order"] = 99;
        input["binding_image_point"] = json::array({132, 75});

        const std::string bindingFile = GenerateBindings(profile, originalDirectory, generation);
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

        const std::string fileName = GeneratedProfileFile(generation);
        const std::string outputPath = GeneratedDirectory() + "\\" + fileName;
        if (!WriteJsonFile(outputPath, profile)) {
            return {};
        }
        dh::Log("Wrote '%s'", outputPath.c_str());

        GeneratedProfile result;
        result.resource = std::string("{") + dh::kDriverName + "}/input/" + fileName;
        result.controllerType = controllerType;
        return result;
    }

    void ApplyGeneratedProfile(vr::PropertyContainerHandle_t container, const GeneratedProfile& generated) {
        {
            std::lock_guard<std::mutex> lock(g_profileMutex);
            g_generatedProfileResource = generated.resource;
            g_generatedControllerType = generated.controllerType;
        }
        g_inOwnWrite = true;
        vr::VRProperties()->SetStringProperty(container, vr::Prop_ControllerType_String, generated.controllerType.c_str());
        vr::VRProperties()->SetStringProperty(container, vr::Prop_InputProfilePath_String, generated.resource.c_str());
        g_inOwnWrite = false;
        dh::Log("Headset now reports controller type '%s' and profile '%s'",
                generated.controllerType.c_str(),
                generated.resource.c_str());
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
            std::string originalProfile =
                vr::VRProperties()->GetStringProperty(container, vr::Prop_InputProfilePath_String);
            dh::Log("Headset activated (id %u), its input profile is '%s'", objectId, originalProfile.c_str());

            // SteamVR reads the bindings once, right after the device is added, so the profile has to
            // be final by the time Activate() returns. A headset driver that sets its own profile a
            // moment later (CustomHeadsetOpenVR does) would be too late, hence this setting.
            const std::string configured = dh::Narrow(dh::IniString(ConfigFile(), L"driver", L"base_profile", L""));
            if (!configured.empty()) {
                // The default config names the CustomHeadsetOpenVR profile. Without that driver the
                // file does not exist: fall back to the headset's own profile instead of generating
                // one without the headset's inputs.
                const std::string configuredPath = ResolveResourcePath(configured, {});
                if (!configuredPath.empty() && GetFileAttributesW(dh::Widen(configuredPath).c_str()) != INVALID_FILE_ATTRIBUTES) {
                    dh::Log("Using the base profile from config.ini: '%s'", configured.c_str());
                    originalProfile = configured;
                } else {
                    dh::Log("The base profile '%s' from config.ini does not exist here, using the headset's own "
                            "profile '%s'",
                            configured.c_str(),
                            originalProfile.c_str());
                }
            }

            const GeneratedProfile generated = GenerateProfile(originalProfile);
            if (!generated.ok()) {
                dh::Log("Could not generate the extended input profile, leaving the headset untouched");
                return status;
            }

            vr::VRInputComponentHandle_t component = vr::k_ulInvalidInputComponentHandle;
            const auto inputError = vr::VRDriverInput()->CreateBooleanComponent(container, kInputComponent, &component);
            if (inputError != vr::VRInputError_None) {
                dh::Log("CreateBooleanComponent('%s') failed: %d", kInputComponent, static_cast<int>(inputError));
                return status;
            }

            ApplyGeneratedProfile(container, generated);
            g_container = container;
            g_component = component;
            dh::Log("Added '%s' to the headset", kInputComponent);
            return status;
        }

        void Deactivate() override {
            g_component = vr::k_ulInvalidInputComponentHandle;
            g_container = vr::k_ulInvalidPropertyContainer;
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
    // Keeping ownership of the headset's input profile
    //
    // Headset drivers may set their own input profile and controller type after our Activate(); that
    // would replace ours and SteamVR would not reload the bindings. We therefore intercept property
    // writes for the headset and substitute our values, so SteamVR only ever sees ours.
    // ---------------------------------------------------------------------------------------------
    using WritePropertyBatchFn = vr::ETrackedPropertyError (*)(vr::IVRProperties*,
                                                               vr::PropertyContainerHandle_t,
                                                               vr::PropertyWrite_t*,
                                                               uint32_t);
    WritePropertyBatchFn g_originalWritePropertyBatch = nullptr;

    void SubstituteOwnedValue(vr::PropertyWrite_t& entry, std::string& buffer, const std::string& value) {
        buffer = value;
        entry.pvBuffer = const_cast<char*>(buffer.c_str());
        entry.unBufferSize = static_cast<uint32_t>(buffer.size() + 1);
    }

    vr::ETrackedPropertyError HookedWritePropertyBatch(vr::IVRProperties* properties,
                                                      vr::PropertyContainerHandle_t container,
                                                      vr::PropertyWrite_t* batch,
                                                      uint32_t entryCount) {
        if (!g_inOwnWrite && batch && container != vr::k_ulInvalidPropertyContainer &&
            container == g_container.load()) {
            std::lock_guard<std::mutex> lock(g_profileMutex);
            if (!g_generatedProfileResource.empty()) {
                for (uint32_t i = 0; i < entryCount; i++) {
                    vr::PropertyWrite_t& entry = batch[i];
                    if (entry.prop == vr::Prop_InputProfilePath_String && entry.pvBuffer) {
                        const std::string incoming(static_cast<const char*>(entry.pvBuffer));
                        if (incoming != g_generatedProfileResource) {
                            dh::Log("Keeping our input profile, the headset driver tried to set '%s'",
                                    incoming.c_str());
                            SubstituteOwnedValue(entry, g_ownedProfileBuffer, g_generatedProfileResource);
                        }
                    } else if (entry.prop == vr::Prop_ControllerType_String && entry.pvBuffer) {
                        const std::string incoming(static_cast<const char*>(entry.pvBuffer));
                        if (incoming != g_generatedControllerType) {
                            SubstituteOwnedValue(entry, g_ownedTypeBuffer, g_generatedControllerType);
                        }
                    }
                }
            }
        }
        return g_originalWritePropertyBatch(properties, container, batch, entryCount);
    }

    bool HookWritePropertyBatch() {
        vr::EVRInitError error = vr::VRInitError_None;
        void* properties = vr::VRDriverContext()->GetGenericInterface(vr::IVRProperties_Version, &error);
        if (!properties || error != vr::VRInitError_None) {
            dh::Log("IVRProperties is not available (%d)", static_cast<int>(error));
            return false;
        }
        void** vtable = *reinterpret_cast<void***>(properties);
        void* target = vtable[1]; // WritePropertyBatch
        if (MH_CreateHook(target,
                          reinterpret_cast<void*>(&HookedWritePropertyBatch),
                          reinterpret_cast<void**>(&g_originalWritePropertyBatch)) != MH_OK ||
            MH_EnableHook(target) != MH_OK) {
            dh::Log("Could not hook IVRProperties::WritePropertyBatch");
            return false;
        }
        return true;
    }

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
        RemoveGeneratedFiles();

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

        if (!HookWritePropertyBatch()) {
            Log("Continuing without property write protection: another driver may replace our profile");
        }

        Log("Headset shim installed");
        return true;
    }

    bool IsHmdShimReady() {
        return g_component != vr::k_ulInvalidInputComponentHandle;
    }

    void MaintainHmdProfile() {
        const vr::PropertyContainerHandle_t container = g_container;
        if (container == vr::k_ulInvalidPropertyContainer) {
            return;
        }

        const std::string currentProfile =
            vr::VRProperties()->GetStringProperty(container, vr::Prop_InputProfilePath_String);
        const std::string currentType =
            vr::VRProperties()->GetStringProperty(container, vr::Prop_ControllerType_String);
        {
            std::lock_guard<std::mutex> lock(g_profileMutex);
            if (currentProfile.empty()) {
                return;
            }
            if (currentProfile == g_generatedProfileResource && currentType == g_generatedControllerType) {
                return;
            }
        }

        // The headset driver replaced our profile (it often sets its own a moment after Activate,
        // and again whenever its settings change). Extend whatever it set now and put ours back.
        Log("The headset switched to '%s' (type '%s'), extending that profile instead",
            currentProfile.c_str(),
            currentType.c_str());
        const GeneratedProfile generated = GenerateProfile(currentProfile);
        if (!generated.ok()) {
            return;
        }
        ApplyGeneratedProfile(container, generated);
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
