# SteamVR-DesktopHotkey

Open and close the **SteamVR desktop view** with one keyboard shortcut (default `Ctrl+Alt+D`) — no VR
controllers needed.

Works with any SteamVR headset, including Pimax headsets running
[CustomHeadsetOpenVR](https://github.com/sboys3/CustomHeadsetOpenVR).

> Status: **experimental**. Tested with SteamVR 2.15, a Pimax Dream Air (Lighthouse) and CustomHeadsetOpenVR.

## How to install

1. **Download** the newest `SteamVR-DesktopHotkey-….zip` from the
   [Releases page](https://github.com/hypeapps/SteamVR-DesktopHotkey/releases/latest).
2. **Unpack** it to a folder where it can stay for good, for example `C:\VR\SteamVR-DesktopHotkey`.
   Not the *Downloads* folder: SteamVR loads the add-on from this folder every time it starts.
3. **Close SteamVR** completely.
4. **Double-click `install.bat`** in that folder. A window opens, shows what it did and waits for a key press.
5. **Start SteamVR. Enjoy.**

That's it — the shortcut works whenever SteamVR is running.

**Not a Pimax headset?** Nothing to change — it works as it is. (The settings file mentions
CustomHeadsetOpenVR; without it, that line is simply ignored.)

## How to use

| Function | How to |
|---|---|
| Open the desktop view | Press `Ctrl+Alt+D` |
| Close the dashboard | Look **away** from the dashboard (to the side or at the floor) and press `Ctrl+Alt+D` |
| Click (like a left mouse click or a controller trigger) | Look at what you want to click on the dashboard and press `Ctrl+Alt+D` |

## Change the shortcut

1. Close SteamVR.
2. Open `desktop_hotkey\config.ini` (inside the install folder) with Notepad.
3. Change the line `keys=Ctrl+Alt+D`, for example to `keys=Ctrl+Shift+F12` or `keys=NumpadDivide`,
   and save the file.
4. Start SteamVR.

You can combine `Ctrl`, `Alt`, `Shift` and `Win` with one key: a letter, a digit, `F1`–`F24`, `Home`, `End`,
`Insert`, `Delete`, `PageUp`, `PageDown`, `Pause`, `NumpadDivide`, … A single character such as `/` works
too.

Pick a combination you don't use for anything else: while SteamVR runs, the shortcut belongs to this
add-on and other programs (games included) will not receive it. For example with `keys=/` you cannot type
`/` anywhere while SteamVR runs.

## Update

1. Close SteamVR.
2. Download and unpack the new zip, and copy everything over your install folder, **except
   `desktop_hotkey\config.ini`** (keep yours, so your settings stay).
3. Start SteamVR.

## Uninstall

1. Close SteamVR.
2. Double-click `uninstall.bat` in the install folder.
3. Delete the folder.

## All settings

All settings are in `desktop_hotkey\config.ini`. Restart SteamVR after changing them.

| Setting | Default | Meaning |
|---|---|---|
| `[hotkey] enabled` | `1` | `0` turns the shortcut off (the command line below still works) |
| `[hotkey] keys` | `Ctrl+Alt+D` | The shortcut, see *Change the shortcut* |
| `[dashboard] desktop_overlay_key` | `system.desktop.1` | Which page the dashboard opens on. `system.desktop.1` = the Desktop page (monitor 1). Empty = the page that was open last |
| `[dashboard] when_open` | `close` | `close`, or `desktop_then_close` (if another page is open, switch to the Desktop page first; press again to close) |
| `[driver] start_helper` | `1` | Start the shortcut program together with SteamVR |
| `[driver] press_duration_ms` | `120` | How long the virtual button is held, in milliseconds |
| `[driver] base_profile` | `{CustomHeadsetOpenVR}/input/pimaxhmd_profile.json` | For Pimax headsets with CustomHeadsetOpenVR. Other headsets can leave it or make it empty (`base_profile=`); see the comments in the file |

## Command line (AutoHotkey, Stream Deck, mouse buttons, …)

Instead of the built-in shortcut you can start the program in `desktop_hotkey\bin\win64` yourself:

```
desktop_hotkey_helper.exe --toggle    open the desktop view / close the dashboard
desktop_hotkey_helper.exe --open      open the desktop view
desktop_hotkey_helper.exe --close     close the dashboard (look away from it)
desktop_hotkey_helper.exe --press     press the virtual button once (for testing)
desktop_hotkey_helper.exe --probe     print diagnostics
```

## Troubleshooting

**Antivirus disclaimer**

- If Windows or your antivirus warns about the files: they are not digitally signed yet, which makes
new programs look suspicious, but this repo is fully open source so you can read the whole code, and ensure that there is no harm there.

**The shortcut does nothing**

- Check that **desktop_hotkey** is **On** in SteamVR → Settings → Startup/Shutdown → Manage Add-ons.
- Open `%LOCALAPPDATA%\SteamVR-DesktopHotkey\helper.log` (paste it into the Explorer address bar).
  It should contain `Hotkey registered: …`. If it says `RegisterHotKey(...) failed`, another program already
  uses that shortcut — pick another one.

**It opens, but it does not close**

- Make sure you look away from the dashboard panel while pressing the shortcut; on the panel it clicks.
- Check the SteamVR log (see below): there should be a line `Added '/input/desktop_hotkey/click' to the headset`.

**It opens on the wrong page**

With SteamVR running, open the Desktop page in the dashboard, then run in PowerShell inside
`desktop_hotkey\bin\win64`:

```powershell
Start-Process .\desktop_hotkey_helper.exe -ArgumentList '--probe' -Wait
```

The key marked `ACTIVE` goes into `desktop_overlay_key`.

**SteamVR log (for bug reports)**

Every line of the add-on starts with `[desktop_hotkey]`:

```powershell
Select-String -Path "C:\Program Files (x86)\Steam\logs\vrserver.txt" -Pattern "desktop_hotkey|generated_hmd" | Select-Object -Last 30
```

(Adjust the path if Steam is installed elsewhere.) A healthy start shows, in this order: `Build …`,
`Config: …`, `Shimming headset …`, `Extending the headset input profile …`, `Headset now reports controller
type '…_dh' …`, `Added '/input/desktop_hotkey/click' to the headset`, and a SteamVR line
`openvr.component.vrcompositor (…_dh) attempting to load default config from …generated_hmd_bindings_vrcompositor_1.json`.

- No `Shimming headset` line: the add-on loaded after the headset driver; `resources/settings/default.vrsettings`
  must keep its high `loadPriority`.
- `Keeping our input profile, the headset driver tried to set '…'`: put that value into `base_profile`.

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
   (`/input/desktop_hotkey`) and matching dashboard bindings, and points the headset at the copy. All inputs
   of the original profile are kept. The copy gets its own controller type (`<original>_dh`) so that SteamVR
   loads its bindings, and a hook on `IVRProperties::WritePropertyBatch` keeps a headset driver from replacing
   it later. The generated files live in `desktop_hotkey/resources/input/generated_*.json`.
   The driver also starts the helper together with SteamVR (and restarts it if it crashes).
2. **Helper** (`desktop_hotkey_helper.exe`) registers the shortcut with the standard `RegisterHotKey` API
   (no keyboard hooks, nothing is sent to games) and, when it is pressed:
   - dashboard closed → opens it on the Desktop page (`ShowDashboard`);
   - dashboard open → asks the driver to press the extra input.

The extra input is bound the same way
[SteamVR-Dashboard-KeyboardNav](https://github.com/mbucchia/SteamVR-Dashboard-KeyboardNav) binds its button:
`/actions/system/in/opendashboard` (opens the dashboard) and `/actions/lasermouse/in/leftclick` with the
pointer on the headset pose (a click aimed with your head; clicking empty space next to the panel closes
the dashboard).

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
