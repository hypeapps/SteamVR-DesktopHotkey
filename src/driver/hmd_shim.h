// SteamVR-DesktopHotkey - headset driver shim
// SPDX-License-Identifier: MIT
#pragma once

namespace dh {

    // Installs a hook on IVRServerDriverHost::TrackedDeviceAdded so that the headset device
    // registered by whatever HMD driver is in use gets wrapped by our shim. The shim adds one extra
    // input (/input/desktop_hotkey) to the headset, keeping every input the original driver declared.
    bool InstallHmdShim();

    // Presses and releases the extra input. Returns false when the headset is not shimmed (yet).
    bool PressHotkeyInput(int pressDurationMs);

    // True once the shimmed headset has been activated and our input exists.
    bool IsHmdShimReady();

} // namespace dh
