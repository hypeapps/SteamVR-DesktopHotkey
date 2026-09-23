// SteamVR-DesktopHotkey - headset driver shim
// SPDX-License-Identifier: MIT
#pragma once

namespace dh {

    // Installs the hooks that wrap the headset device registered by whatever HMD driver is in use.
    // The shim adds one extra input (/input/desktop_hotkey) to the headset, keeping every input the
    // original driver declared. See hmd_shim.cpp for the details.
    bool InstallHmdShim();

    // Presses and releases the extra input. It opens the dashboard when it is closed, and closes it
    // when it is open and the gaze is not on the dashboard panel (on the panel it clicks instead).
    // Returns false when the headset is not shimmed (yet).
    bool PressHotkeyInput(int pressDurationMs);

    // True once the shimmed headset has been activated and our input exists.
    bool IsHmdShimReady();

    // Safety net: re-applies our generated input profile if it was replaced anyway. Call regularly.
    void MaintainHmdProfile();

} // namespace dh
