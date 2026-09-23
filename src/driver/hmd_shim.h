// SteamVR-DesktopHotkey - headset driver shim
// SPDX-License-Identifier: MIT
#pragma once

namespace dh {

    // Installs a hook on IVRServerDriverHost::TrackedDeviceAdded so that the headset device
    // registered by whatever HMD driver is in use gets wrapped by our shim. The shim adds one extra
    // input (/input/desktop_hotkey) to the headset, keeping every input the original driver declared.
    bool InstallHmdShim();

    // Presses and releases an input on the headset. With useSystemButton the headset's own system
    // button is used (SteamVR handles it natively: it opens AND closes the dashboard); otherwise the
    // extra input this shim added is used. Returns false when the headset is not shimmed (yet).
    bool PressHotkeyInput(int pressDurationMs, bool useSystemButton);

    // True once the shimmed headset has been activated and our input exists.
    bool IsHmdShimReady();

    // Re-applies our generated input profile if the headset driver has replaced it (it may set its
    // own profile after our Activate(), or whenever its settings change). Cheap, call it regularly.
    void MaintainHmdProfile();

} // namespace dh
