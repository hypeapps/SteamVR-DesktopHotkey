// SteamVR-DesktopHotkey - driver logging
// SPDX-License-Identifier: MIT
#pragma once

#include <openvr_driver.h>

#include <cstdarg>
#include <cstdio>

namespace dh {

    inline void Log(const char* format, ...) {
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

} // namespace dh
