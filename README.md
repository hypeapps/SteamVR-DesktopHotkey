# SteamVR-DesktopHotkey

Open the **SteamVR dashboard directly on the Desktop view** and close it again with a single keyboard
shortcut (default `Ctrl+Alt+D`) — no VR controllers needed.

It is a small SteamVR add-on that works alongside other drivers such as
[CustomHeadsetOpenVR](https://github.com/sboys3/CustomHeadsetOpenVR):

- it **extends** the headset's input profile with one extra input instead of replacing it, so every
  input the headset driver provides (volume buttons, taps, proximity sensor, ...) keeps working;
- **no keyboard hooks, no input injection** — the hotkey uses the standard `RegisterHotKey` API
  (same as Discord, OBS, Steam overlay); nothing is sent to or injected into games.

> Status: **experimental**. Please report what works on your SteamVR version (see *Testing* below).

## How it works

```
 keyboard ──► desktop_hotkey_helper.exe ──(dashboard closed)──► IVROverlay::ShowDashboard("system.desktop.1")
                     │
                     └──(dashboard open)──► named event ──► driver_desktop_hotkey.dll
                                                               └► presses /input/desktop_hotkey on the
                                                                  headset, bound to "open/close dashboard"
```

1. **Driver** (`driver_desktop_hotkey.dll`) hooks `IVRServerDriverHost::TrackedDeviceAdded` and wraps the
   headset device with a shim that forwards every call to the original driver. Once the original driver has
   activated the headset, the shim reads the input profile it set, writes a copy of it with one input added
   (`/input/desktop_hotkey`) plus a dashboard binding for it, and points the headset at that generated
   profile. The generated files live in `desktop_hotkey/resources/input/generated_*.json`.
   The driver also starts the helper together with SteamVR (and restarts it if it crashes).
2. **Helper** (`desktop_hotkey_helper.exe`) registers the hotkey and, when pressed:
   - dashboard closed → opens it on the Desktop page (`ShowDashboard`);
   - dashboard open → asks the driver to press the extra headset input, which closes the dashboard.
   OpenVR has no public "hide dashboard" call, which is why the extra input exists.

Why the headset and not a device of our own: SteamVR only acts on the "open/close dashboard" action when
it comes from `/user/head`. A separate virtual device is registered and bound without any error, but
pressing its button does nothing (tested with the `treadmill`, `stylus` and hand roles on SteamVR 2.15).

## Install

1. Download the latest build: **Actions → latest run → Artifacts → SteamVR-DesktopHotkey**
   (or a release zip if available) and extract it somewhere permanent, e.g. `D:\VR\SteamVR-DesktopHotkey`.
2. **Close SteamVR.**
3. Run `install.bat`.
4. Start SteamVR and make sure **Settings → Startup/Shutdown → Manage Add-ons → desktop_hotkey** is **On**.

Uninstall: close SteamVR, run `uninstall.bat`, delete the folder.

## Configuration

Edit `desktop_hotkey\config.ini` and restart SteamVR:

| Section / key | Default | Meaning |
|---|---|---|
| `[hotkey] enabled` | `1` | `0` disables the built-in hotkey (command line still works) |
| `[hotkey] keys` | `Ctrl+Alt+D` | Modifiers `Ctrl` `Alt` `Shift` `Win` + one key (`A`–`Z`, `0`–`9`, `F1`–`F24`, `Home`, `0x7B`, …) |
| `[dashboard] desktop_overlay_key` | `system.desktop.1` | Overlay key of the Desktop page (`system.desktop.1` = monitor 1 on current SteamVR, `valve.steam.desktop` on older versions); empty = open dashboard on its last page |
| `[dashboard] when_open` | `close` | `close`, or `desktop_then_close` (switch to Desktop first if another page is shown) |
| `[driver] start_helper` | `1` | Start the helper with SteamVR |
| `[driver] press_duration_ms` | `120` | How long the extra headset input is held |
| `[driver] base_profile` | empty | Input profile to extend, e.g. `{CustomHeadsetOpenVR}/input/pimaxhmd_profile.json`. Needed when the headset driver sets its profile after the device is activated — by then SteamVR has already read the bindings. Empty = whatever the headset reports at activation |

Choose an unusual combination: while the helper runs, the shortcut is reserved and other programs
(including games) will not receive it.

## Command line (AutoHotkey, Stream Deck, mouse buttons, …)

```
desktop_hotkey\bin\win64\desktop_hotkey_helper.exe --toggle
desktop_hotkey\bin\win64\desktop_hotkey_helper.exe --open
desktop_hotkey\bin\win64\desktop_hotkey_helper.exe --close
desktop_hotkey\bin\win64\desktop_hotkey_helper.exe --probe
```

`--close` (and closing via `--toggle`) needs the driver to be loaded.

## Testing / troubleshooting

With SteamVR running, open a console in `desktop_hotkey\bin\win64` and run:

```
:: cmd
start /wait desktop_hotkey_helper.exe --probe
```
```powershell
# PowerShell
Start-Process .\desktop_hotkey_helper.exe -ArgumentList '--probe' -Wait
Get-Content "$env:LOCALAPPDATA\SteamVR-DesktopHotkey\helper.log" -Tail 30
```

It prints whether the driver is loaded, whether the hotkey is valid and which overlay keys exist.
Open the Desktop page in the dashboard and run it again — the key marked `ACTIVE` is the one to put in
`desktop_overlay_key`.

Checklist:

1. **Open**: press the hotkey with the dashboard closed → dashboard opens on the Desktop page?
   If it opens on another page, fix `desktop_overlay_key` using `--probe`.
2. **Close**: press the hotkey with the dashboard open → dashboard closes?
   If not, look for `[desktop_hotkey]` lines in `vrserver.txt`: they say whether the headset was shimmed,
   which profile was extended and whether the press reached the headset input.
3. The headset's own buttons and your controllers still work as before.

Logs:

- helper: `%LOCALAPPDATA%\SteamVR-DesktopHotkey\helper.log`
- driver: SteamVR `vrserver.txt` (lines starting with `[desktop_hotkey]`)

## Building

Requirements: Visual Studio 2022 (C++ desktop workload) and CMake ≥ 3.20.

```
cmake -S . -B build -A x64
cmake --build build --config Release
cmake --install build --config Release --prefix dist/SteamVR-DesktopHotkey
```

Every push is built by GitHub Actions; pushing a tag `v*` publishes a release zip.

## Credits

- [OpenVR SDK](https://github.com/ValveSoftware/openvr) (BSD-3-Clause) — headers, import library and
  `openvr_api.dll` in `third_party/openvr`.
- [MinHook](https://github.com/TsudaKageyu/minhook) (BSD-2-Clause) — used to hook `TrackedDeviceAdded`.
- [nlohmann/json](https://github.com/nlohmann/json) (MIT) — reading and writing the input profiles.
- The driver shimming technique, and the idea of driving the dashboard from a driver-side input, come from
  [mbucchia/SteamVR-Dashboard-KeyboardNav](https://github.com/mbucchia/SteamVR-Dashboard-KeyboardNav) (MIT).

## License

MIT
