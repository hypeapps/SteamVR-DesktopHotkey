cmake_minimum_required(VERSION 3.20)
project(SteamVR-DesktopHotkey LANGUAGES CXX)

if(NOT WIN32)
    message(FATAL_ERROR "SteamVR-DesktopHotkey is Windows-only.")
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
# Static CRT: no Visual C++ Redistributable needed on the target machine.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

set(OPENVR_DIR ${CMAKE_CURRENT_SOURCE_DIR}/third_party/openvr)
set(DRIVER_OUT desktop_hotkey)

add_compile_definitions(UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX _CRT_SECURE_NO_WARNINGS)
if(MSVC)
    add_compile_options(/W4 /permissive- /utf-8)
endif()

# --- SteamVR driver: driver_desktop_hotkey.dll ------------------------------------------------
add_library(driver_desktop_hotkey SHARED src/driver/driver.cpp)
set_target_properties(driver_desktop_hotkey PROPERTIES PREFIX "")
target_include_directories(driver_desktop_hotkey PRIVATE ${OPENVR_DIR}/headers)

# --- Helper process: desktop_hotkey_helper.exe ------------------------------------------------
add_library(openvr_api SHARED IMPORTED)
set_target_properties(openvr_api PROPERTIES
    IMPORTED_IMPLIB ${OPENVR_DIR}/lib/win64/openvr_api.lib
    IMPORTED_LOCATION ${OPENVR_DIR}/bin/win64/openvr_api.dll
    INTERFACE_INCLUDE_DIRECTORIES ${OPENVR_DIR}/headers)

add_executable(desktop_hotkey_helper WIN32 src/helper/helper.cpp)
target_link_libraries(desktop_hotkey_helper PRIVATE openvr_api)
if(MINGW)
    target_link_options(desktop_hotkey_helper PRIVATE -municode)
endif()

# --- Package layout (cmake --install build --prefix dist) -------------------------------------
install(TARGETS driver_desktop_hotkey desktop_hotkey_helper
    RUNTIME DESTINATION ${DRIVER_OUT}/bin/win64
    LIBRARY DESTINATION ${DRIVER_OUT}/bin/win64)
install(FILES ${OPENVR_DIR}/bin/win64/openvr_api.dll DESTINATION ${DRIVER_OUT}/bin/win64)
install(DIRECTORY driver_files/desktop_hotkey/ DESTINATION ${DRIVER_OUT})
install(FILES
    scripts/install.bat scripts/install.ps1
    scripts/uninstall.bat scripts/uninstall.ps1
    README.md LICENSE
    DESTINATION .)
install(FILES ${OPENVR_DIR}/LICENSE DESTINATION licenses RENAME openvr-LICENSE.txt)
