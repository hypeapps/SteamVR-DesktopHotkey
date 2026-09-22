# SteamVR-DesktopHotkey

Open the **SteamVR dashboard directly on the Desktop view** and close it again with a single keyboard
shortcut (default `Ctrl+Alt+D`) — no VR controllers needed.

It is a small standalone SteamVR add-on designed to coexist with other drivers
(e.g. [CustomHeadsetOpenVR](https://github.com/sboys3/CustomHeadsetOpenVR)):

- **no hooks / no driver shimming** — the headset driver is never touched;
- **no keyboard hooks, no input injection** — the hotkey uses the standard `RegisterHotKey` API
  (same as Discord, OBS, Steam overlay); nothing is sent to or injected into games;
- only public OpenVR APIs.

> Status: **experimental**. Please report what works on your SteamVR version (see *Testing* below).

## How it works

```
 keyboard ──► desktop_hotkey_helper.exe ──(dashboard closed)──► IVROverlay::ShowDashboard("system.desktop.1")
                     │
                     └──(dashboard open)──► named event ──► driver_desktop_hotkey.dll
                                                               └► presses the "system" button of a
                                                                  virtual, never-tracked device
                                                                  (bound to "open/close dashboard")
```

1. **Driver** (`driver_desktop_hotkey.dll`) registers one virtual device with a single `/input/system` button.
   It has no pose and no render model, and uses the `treadmill` controller role, so SteamVR and games
   never treat it as a hand controller. Its default SteamVR dashboard binding is *open/close dashboard*.
   The driver also starts the helper together with SteamVR (and restarts it if it crashes).
2. **Helper** (`desktop_hotkey_helper.exe`) registers the hotkey and, when pressed:
   - dashboard closed → opens it on the Desktop page (`ShowDashboard`);
   - dashboard open → asks the driver to press the virtual button, which closes the dashboard.
   OpenVR has no public "hide dashboard" call, which is why the virtual button exists.

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
| `[driver] role` | `treadmill` | Input path of the virtual device: `treadmill`, `stylus`, `opt_out`, or (testing only) `left` / `right` |
| `[driver] press_duration_ms` | `80` | How long the virtual button is held |
| `[driver] hand_priority` | `-1000000` | Hand selection priority; keep it low so real controllers always win (set `0` only when testing `role=left`/`right`) |
| `[driver] report_pose` | `1` | Report a static pose for the virtual device (some SteamVR versions ignore input from poseless devices); it is still never drawn |

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
   If not, check *Settings → Controllers → Manage Controller Bindings* for the **Desktop Hotkey** device,
   or try `role=stylus`.
3. Headset buttons and your controllers still work as before.

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
- Idea of a driver-side virtual button for dashboard navigation inspired by
  [mbucchia/SteamVR-Dashboard-KeyboardNav](https://github.com/mbucchia/SteamVR-Dashboard-KeyboardNav) (MIT).

## License

MIT
