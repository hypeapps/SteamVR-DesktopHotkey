# SteamVR-DesktopHotkey

Open the **SteamVR dashboard directly on the Desktop view** and close it again with a single keyboard
shortcut (default `Ctrl+Alt+D`) — no VR controllers needed.

It is a small SteamVR add-on that works alongside other drivers such as
[CustomHeadsetOpenVR](https://github.com/sboys3/CustomHeadsetOpenVR):

- it **extends** the headset's input profile with one extra input instead of replacing it, so every
  input the headset driver provides (volume buttons, taps, proximity sensor, ...) keeps working;
- **no keyboard hooks, no input injection** — the hotkey uses the standard `RegisterHotKey` API
  (same as Discord, OBS, Steam overlay); nothing is sent to or injected into games.

> Status: **experimental**. Tested with SteamVR 2.15, a Pimax Dream Air (Lighthouse) and CustomHeadsetOpenVR.

## Usage

| Dashboard | You look at | `Ctrl+Alt+D` does |
|---|---|---|
| closed | anywhere | opens the dashboard on the Desktop page |
| open | **away from** the dashboard panel | closes the dashboard |
| open | the dashboard panel | clicks what you look at (like a controller trigger) |

So to close the dashboard, turn your head away from the panel (to the side, at the floor, ...) and
press the hotkey.

## How it works

```
 keyboard ──► desktop_hotkey_helper.exe ──(dashboard closed)──► IVROverlay::ShowDashboard("system.desktop.1")
                     │
                     └──(dashboard open)──► named event ──► driver_desktop_hotkey.dll
                                                               └► presses /input/desktop_hotkey on the headset:
                                                                  a head-aimed click on empty space
                                                                  next to the panel closes the dashboard
```

1. **Driver** (`driver_desktop_hotkey.dll`) wraps the headset device registered by the headset driver
   (hook on `IVRServerDriverHost::TrackedDeviceAdded`; every call is forwarded unchanged). When the headset
   is activated, it takes the headset's input profile, writes a copy with one extra input
   (`/input/desktop_hotkey`) and matching dashboard bindings, and points the headset at the copy.
   The copy gets its own controller type (`<original>_dh`) so that SteamVR loads its bindings, and a hook on
   `IVRProperties::WritePropertyBatch` keeps a headset driver from replacing it later.
   The generated files live in `desktop_hotkey/resources/input/generated_*.json`.
   The driver also starts the helper together with SteamVR (and restarts it if it crashes).
2. **Helper** (`desktop_hotkey_helper.exe`) registers the hotkey and, when pressed:
   - dashboard closed → opens it on the Desktop page (`ShowDashboard`);
   - dashboard open → asks the driver to press the extra input.

The extra input is bound the same way
[SteamVR-Dashboard-KeyboardNav](https://github.com/mbucchia/SteamVR-Dashboard-KeyboardNav) binds its button:

- `/actions/system/in/opendashboard` — opens the dashboard;
- `/actions/lasermouse/in/leftclick` with the pointer on the headset pose — a click aimed with your head.
  Clicking empty space next to the dashboard panel closes the dashboard.

### What does not work (and why this design)

Findings from SteamVR 2.15, so nobody has to repeat them:

- OpenVR has no public call that hides the dashboard; `ShowDashboard` can only open it.
- The `opendashboard` action **only opens** the dashboard, it never closes it — whichever device it comes from.
- A separate virtual device (roles `treadmill`, `stylus`, left hand) is registered and bound without any
  error, but its button does not close the dashboard either: closing needs the head-aimed laser click.
- SteamVR reads a device's bindings right after the device is added. A headset driver that sets its own input
  profile a moment later (CustomHeadsetOpenVR does) would therefore replace ours too late for SteamVR to
  notice — hence `base_profile` and the property-write hook.

## Install

1. Download the latest build: **Actions → latest run → Artifacts → SteamVR-DesktopHotkey**
   (or a release zip if available) and extract it somewhere permanent, e.g. `D:\VR\SteamVR-DesktopHotkey`.
2. **Close SteamVR.**
3. Run `install.bat`.
4. If your headset driver sets its input profile late (CustomHeadsetOpenVR does), set `base_profile` in
   `desktop_hotkey\config.ini` — see *Configuration*.
5. Start SteamVR and make sure **Settings → Startup/Shutdown → Manage Add-ons → desktop_hotkey** is **On**.

Updating: close SteamVR and replace everything in the folder **except `desktop_hotkey\config.ini`**.

Uninstall: close SteamVR, run `uninstall.bat`, delete the folder.

## Configuration

Edit `desktop_hotkey\config.ini` and restart SteamVR:

| Section / key | Default | Meaning |
|---|---|---|
| `[hotkey] enabled` | `1` | `0` disables the built-in hotkey (command line still works) |
| `[hotkey] keys` | `Ctrl+Alt+D` | Modifiers `Ctrl` `Alt` `Shift` `Win` + one key (`A`–`Z`, `0`–`9`, `F1`–`F24`, `Home`, `0x7B`, …) |
| `[dashboard] desktop_overlay_key` | `system.desktop.1` | Overlay key of the Desktop page (`system.desktop.1` = monitor 1 on current SteamVR, `valve.steam.desktop` on older versions); empty = open the dashboard on its last page |
| `[dashboard] when_open` | `close` | `close`, or `desktop_then_close` (switch to Desktop first if another page is shown) |
| `[driver] start_helper` | `1` | Start the helper with SteamVR |
| `[driver] press_duration_ms` | `120` | How long the headset input is held |
| `[driver] base_profile` | empty | Input profile to extend. Empty = whatever the headset reports when it is activated. **CustomHeadsetOpenVR with a Pimax headset:** `{CustomHeadsetOpenVR}/input/pimaxhmd_profile.json` |

Choose an unusual key combination: while the helper runs, the shortcut is reserved and other programs
(including games) will not receive it.

## Command line (AutoHotkey, Stream Deck, mouse buttons, …)

```
desktop_hotkey\bin\win64\desktop_hotkey_helper.exe --toggle
desktop_hotkey\bin\win64\desktop_hotkey_helper.exe --open
desktop_hotkey\bin\win64\desktop_hotkey_helper.exe --close
desktop_hotkey\bin\win64\desktop_hotkey_helper.exe --press
desktop_hotkey\bin\win64\desktop_hotkey_helper.exe --probe
```

`--close`, closing via `--toggle` and `--press` need the driver to be loaded. `--press` presses the headset
input without looking at the dashboard state (useful to check the input and its bindings).

## Troubleshooting

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

It prints the build, whether the driver is loaded, whether the hotkey is valid, the tracked devices and
which overlay keys exist. Open the Desktop page in the dashboard and run it again — the key marked `ACTIVE`
is the one to put in `desktop_overlay_key`.

The driver writes to the SteamVR log (`Steam\logs\vrserver.txt`), every line starts with `[desktop_hotkey]`:

```powershell
Select-String -Path "C:\Program Files (x86)\Steam\logs\vrserver.txt" -Pattern "desktop_hotkey|generated_hmd" | Select-Object -Last 30
```

A healthy start shows, in this order: `Build ...` and `Config: ...` (check that they match what you
installed), `Shimming headset ...`, `Extending the headset input profile ...`, `Headset now reports
controller type '..._dh' ...`, `Added '/input/desktop_hotkey/click' to the headset`, and a SteamVR line
`openvr.component.vrcompositor (..._dh) attempting to load default config from ...generated_hmd_bindings_vrcompositor_1.json`.

- No `Shimming headset` line: the driver loaded after the headset driver. `resources/settings/default.vrsettings`
  must keep its high `loadPriority`.
- `its input profile is ...` shows a profile that is later replaced (`Keeping our input profile, the headset
  driver tried to set '...'`): put that profile into `base_profile`.

Helper log: `%LOCALAPPDATA%\SteamVR-DesktopHotkey\helper.log`.

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
- [MinHook](https://github.com/TsudaKageyu/minhook) (BSD-2-Clause) — hooks in the SteamVR server.
- [nlohmann/json](https://github.com/nlohmann/json) (MIT) — reading and writing the input profiles.
- The driver shimming technique and the button bindings come from
  [mbucchia/SteamVR-Dashboard-KeyboardNav](https://github.com/mbucchia/SteamVR-Dashboard-KeyboardNav) (MIT).

## License

MIT
