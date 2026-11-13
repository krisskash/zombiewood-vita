# Zombiewood — PS Vita port (loader)

A loader that runs the Android version of **Zombiewood 1.5.3** natively on the PS Vita. It loads the
game's own ARMv7 libraries into memory, patches their Android imports with Vita equivalents, and
renders through vitaGL.

**No game files are included.** The VPK contains the loader plus a LiveArea icon and background
made from the game's artwork; no code, models, sounds or other game data. You need your own
legally obtained copy of Zombiewood 1.5.3 for Android. Zombiewood is © Gameloft; this project is
not affiliated with or endorsed by Gameloft.

## Status
Playable from start to finish of a level on real hardware (PS Vita, HENkaku/Ensō).

- Menus, gameplay, saves, the in-game shop (local; no online services).
- Average ~33–40 fps in busy levels; short hitches remain (see Known issues).
- Auto-sleep and resume work.

## Requirements
- A PS Vita or PS TV with HENkaku/Ensō, [kubridge](https://github.com/TheOfficialFloW/kubridge)
  installed, and VitaShell.
- `libshacccg.suprx` in `ur0:data/`: the shader compiler vitaGL needs, extracted from your own
  console's firmware (the Vita troubleshooting guide covers this; most vitaGL ports need it).
- Zombiewood **1.5.3** (version code 15325) for Android. Other versions will not load: the loader
  patches fixed code offsets of this exact build and checks the files before starting.

| File | Where it comes from | Check |
|---|---|---|
| `libZombieWood.so` | APK: `lib/armeabi-v7a/` | SHA-1 `918dca729e0bb23fbb894a68e05198370c4962c0` |
| `libgenerator.so` | APK: `lib/armeabi-v7a/` | SHA-1 `485529aaf555047c0571424cf201f6224019415c` |
| `main.1501.com.gameloft.android.ANMP.GloftZOHM.obb` | your Android device: `Android/obb/com.gameloft.android.ANMP.GloftZOHM/` | 289,401,986 bytes |
| `patch.15325.com.gameloft.android.ANMP.GloftZOHM.obb` | same folder | 1,479,926 bytes |

The APK this was tested with (`com.gameloft.android.ANMP.GloftZOHM`, 1.5.3, 33,430,146 bytes) has
SHA-1 `14fa6e1e7840465c4070f81e75d9abade0022f92`. The OBB files are not inside the APK; the game
downloads them on Android, so copy them from a device where it is installed.

## Install
The game reads most of its data unpacked, so the APK and OBBs have to be prepared on a PC first.
`tools/prepare_data.py` does this from your own files (Python 3, standard library only, nothing
is downloaded).

1. **Prepare the data.**
   - Windows: put `tools/prepare_data.bat`, `tools/prepare_data.py`, your APK and both `.obb`
     files in one folder and double-click `prepare_data.bat`. Needs Python 3 from python.org.
   - Linux / macOS:
     `python3 prepare_data.py --apk <your.apk> --obb-dir <folder with the .obb files>`

   It checks every file against the table above, extracts the libraries from the APK and unpacks
   the OBBs into a `zombiewood` folder (586 files, about 570 MB).
2. Copy the **contents** of that `zombiewood` folder to `ux0:data/zombiewood/` on the Vita.
3. Install `zombiewood.vpk` with VitaShell.
4. Launch **Zombiewood**. Every start checks the libraries and OBBs, and every unpacked file
   against `zw_data.txt` (written by the prep script). A missing, different or partly copied file
   shows an error screen naming it; copy the folder again.

## Controls
| Vita | Action |
|---|---|
| Left / right stick | Move / aim |
| Square / Triangle / Circle | Power-ups 1 / 2 / 3 (badges show on screen) |
| Cross | Tap the centre of the screen |
| START | Pause / resume |
| Touch screen | Works as on Android |

Options › Notifications › **SHOW STICKS** toggles the on-screen sticks.

## Known issues
- A ~60 ms hitch about every 20 s, when the HUD's Flash engine collects garbage.
- Enemies spawning mid-fight can cause an occasional 70–270 ms hitch while their data is read
  from the OBB.
- Online features (leaderboards, social, ads) are disabled.

## Building
Needs [VitaSDK](https://vitasdk.org) with vitaGL, vitashark, mathneon and kubridge_stub installed
(e.g. through vdpm).

```bash
bash build.sh
```

The VPK is written to `build/zombiewood.vpk` (title ID `ZMBW00001`). The LiveArea art is not in
git: put your own `sce_sys/icon0.png` and `sce_sys/livearea/contents/{bg.png,startup.png,template.xml}`
in the repository root to have them packed, otherwise the VPK gets the default bubble. Measurement options
(`ZW_PERIODIC_LOG`, `ZW_CALLPROF`, A/B switches) are off by default; pass them with
`ZW_CMAKE_ARGS='-DZW_DEFS=ZW_PERIODIC_LOG=1' bash build.sh`. Never ship a measurement build.

## Reporting bugs
Open an issue with: your firmware and CFW, the APK version, what happened, and
`ux0:data/zombiewood/debug_v2.log`.

## AI disclosure
This port was made with substantial help from an AI assistant (Claude), used for code, debugging
and performance work. Every build was tested on real PS Vita hardware.

## Credits
- [TheFloW](https://github.com/TheOfficialFloW) (Andy Nguyen): `so_util` from
  [gtasa_vita](https://github.com/TheOfficialFloW/gtasa_vita) (MIT, see `LICENSE.so_util`), whose
  loader design this port follows, and kubridge.
- Andy Nguyen, [Rinnegatamante](https://github.com/Rinnegatamante) and
  [Volodymyr Atamanenko](https://github.com/v-atamanenko): the Modern Combat 3 Vita port, from which
  the bionic pthread wrappers and several Gameloft-specific approaches were adapted (MIT, see
  `LICENSE.mc3`).
- Rinnegatamante: vitaGL, vitashark and mathneon.
- The [VitaSDK](https://vitasdk.org) team, and the authors of zlib and libpng.

## License
The loader's own code is GPL-3.0 (see `LICENSE`). `so_util.c` / `so_util.h` and the parts adapted
from the Modern Combat 3 port remain under their MIT licenses (`LICENSE.so_util`, `LICENSE.mc3`).
Zombiewood and all of its assets belong to Gameloft and are not covered by this license.
