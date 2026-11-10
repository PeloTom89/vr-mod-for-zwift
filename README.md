# VR mod for Zwift

Ride Zwift in a VR headset.

This is an unofficial mod for the Zwift app on Windows. Zwift's menus appear on a large virtual screen in front
of you. When your ride starts, you're inside the world: look around, and Zwift's on-screen info (power, speed, map,
riders nearby, turn choices) floats on panels around your handlebars.

**Recommended headset: Meta Quest 3** with the Meta Horizon Link app. That's what the mod is developed and tuned on.

> **Unofficial and not affiliated with Zwift.** Zwift's terms don't allow modifying the app. Use it at your own
> risk, and don't use it in races.
>
> The mod only changes how Zwift is *displayed*. It never touches your power, speed or any other ride data. A Zwift
> update can stop it working until the mod is updated.

## Tested on

- RTX 4090 desktop with a Pimax Crystal Light (early testing)
- RTX 4050 laptop with a Meta Quest 2 over Meta Horizon Link
- RTX 4060 laptop with a Meta Quest 3 over Meta Horizon Link (current, recommended)

## What you need

- **Windows 10 or 11 (64-bit)** and the Zwift app for Windows.
- **An NVIDIA graphics card**, and Zwift must run on it. On a laptop: Windows Settings > System > Display >
  Graphics, add `ZwiftApp.exe` and choose **High performance**.
- **A PC VR headset:**
  - **Meta Quest 3 (recommended)** or Quest 2, with the **Meta Horizon Link** app on your PC (cable or Air Link).
    Once, in Meta Horizon Link: Settings > General > next to OpenXR Runtime, select **Set Meta Horizon Link as
    active** (if it's greyed out, it's already set).
  - **Pimax** headsets with Pimax Play, set as the OpenXR runtime.
  - Other headsets (SteamVR, Virtual Desktop) may work but haven't been tested.

Nothing else needs to be installed.

## Install

1. Download the latest zip from the [Releases page](https://github.com/PeloTom89/vr-mod-for-zwift/releases) and
   unzip it anywhere, for example `C:\vr-mod-for-zwift`. Keep the files together.
2. If Windows blocked the download: right-click `zvr_loader.exe` and `zwift_vr.dll` > Properties > tick
   **Unblock** > OK. Your antivirus may warn about `zvr_loader.exe`, because it loads the mod into Zwift while Zwift
   runs.
3. **Recommended:** VR graphics settings for Zwift. They turn off effects that cost performance or look wrong in a
   headset (shadows, bloom, speed blur and so on). Close Zwift, open PowerShell **as administrator** and run:

   ```
   powershell -ExecutionPolicy Bypass -File C:\vr-mod-for-zwift\tools\apply_zwift_perf_profile.ps1
   ```

   Run it again after a Zwift update. To undo it, run the same command with `-Restore` at the end.

## Ride

1. Turn on your headset and start **Meta Horizon Link** (or Pimax Play) **before** Zwift. On the Quest, connect to
   your PC with Link or Air Link.
2. Double-click `zvr_loader.exe` and accept the administrator prompt. Leave its window open: it loads the mod every
   time Zwift starts.
3. Start Zwift as usual and put your headset on.
   - In Zwift's menus you see Zwift on a virtual screen.
   - When your ride starts, you're in the world. On your PC monitor, Zwift's window grows bigger than the screen
     (this makes the headset picture sharper), so only part of it shows there. That's normal, and it goes back
     after the ride.
   - If the view isn't centred on you, look straight ahead and press **Ctrl+Shift+F1**.

## Keyboard shortcuts

| Keys | What it does |
|---|---|
| Ctrl+Shift+F1 | Re-centre: your view starts from where your head is now |
| Ctrl+Shift+F10 | Switch between the virtual screen and being in the world |
| Ctrl+Shift+F9 | Floating info panels on / off (off = Zwift's info is drawn on the picture as usual) |
| Ctrl+Shift+] / Ctrl+Shift+[ | Move the info panels further out / closer to the centre |
| Ctrl+Shift+Delete | Turn the bigger Zwift window off / on (it turns on by itself when you ride) |

## Settings

Settings live in `zwift_vr.ini`, next to `zvr_loader.exe`. Open it with Notepad, change a value, save, and restart
Zwift. The file explains every setting; these are the ones you're most likely to want:

| Setting | What it does |
|---|---|
| `world_scale` | 1.0 = life size. Raise it if the world feels too small, lower it if it feels too big. |
| `spread` | How far from the centre the info panels sit (default 1.3). |
| `enabled` under `[hud]` | 0 = keep Zwift's info on the picture instead of on floating panels. |
| `distance`, `width` under `[screen]` | Distance and size of the virtual screen in the menus, in metres. |
| `auto_big_window` | 0 = don't enlarge Zwift's window when you ride (your monitor shows all of Zwift, but the headset picture is softer). |
| `eye_resolution` | Sharpness vs. performance. 1.0 = matched to your headset. |

## Tips and troubleshooting

- **The headset shows nothing from Zwift:** start Meta Horizon Link (or Pimax Play) first, check the OpenXR runtime
  setting above, then restart Zwift. The loader can stay open.
- **Meta Horizon Link says "incompatible GPU" after an NVIDIA driver update:** some driver versions break it (621.14
  did; 591.86 works). Go back to the older driver.
- **Use 72 or 90 Hz on the headset.** At 120 Hz, a laptop couldn't send frames to the headset fast enough, which
  caused artifacting.
- **Zwift slows down while the Meta menu is open** in the headset. That's normal and stops when you close it.
- **Reporting a problem:** include the log file, `%LOCALAPPDATA%\zwift-vr\zwift-vr.log`.

## Uninstall

Close `zvr_loader.exe` (Zwift runs normally without it), undo the graphics settings with `-Restore` (see Install),
then delete the folder.

## Building from source

You need Visual Studio 2026 Build Tools with "Desktop development with C++" (including the C++ CMake tools) and Git.
Run `build.cmd`; the loader, the mod and the settings file end up in `build\Release`.

## License

VR mod for Zwift is released under the [MIT License](LICENSE). It includes the OpenXR loader, JsonCpp and MinHook; their
licences are in [THIRD-PARTY-NOTICES.txt](THIRD-PARTY-NOTICES.txt). "Zwift" is a trademark of Zwift, Inc., used only
to say which app this mod works with.
