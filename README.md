# SA-MP Mobile

**English | [简体中文](README.zh-CN.md)**

An Android SA-MP multiplayer client for **GTA: San Andreas Android v2.10** (Java launcher + C++ native library `libSAMPMOBILE.so`), supporting both `arm64-v8a` and `armeabi-v7a` builds.

> See [NOTICE](NOTICE) for copyright, third-party components and compliance information.

![Launcher main screen](images/4.jpg)

---

## Overview

This project continuously evolves from the open-source Alyn SA-MP Mobile architecture. For modern Android (up to **Android 14 / 15, SDK 35**) it delivers **a full launcher rewrite, a rebuilt foundation of the native protocol and engine layers, broad and deep SA-MP / `open.mp` behavioral parity, coexistence of a five-in-one mod ecosystem, and end-to-end stability hardening**.

The client is designed around one principle: **"strictly match PC SA-MP / `open.mp` behavior — never break the multiplayer protocol or anti-cheat contract for the sake of mobile adaptation."** For everything involving core synchronization, combat determination, vehicle state machines, single-player residue suppression and server anti-cheat compatibility, the reference baselines are the `open.mp` upstream source code, official documentation, and disassembly of `libGTASA.so` (arm64 / arm32). Mobile-side engineering adaptation is limited to touch interaction, Android window/process isolation, a high-resolution Chinese font atlas, and modern system security policies.

This project is maintained on an irregular basis, stays fully open and transparent, and contains no hidden logic or private features. The repository ships no game assets or commercial fonts — all runtime files are extracted by users from their own copy of the game. You are welcome to build, modify and fork it; derivative versions must be open-sourced under the same **GNU GPL v3.0** license. If you run into problems, please file an Issue.

---

## Quick Start

**One entry point: `build_tool.bat`** (double-click; pick 中文/English at startup — the choice is remembered). Every routine build operation lives in the menu:

```
1) Bundled APK — game data embedded, install-extract-play (recommended)
2) Plain APK — no embedded data (release/debug)
3) Game-data zip — for manual in-app import (lite/full)
4) Extract GTA:SA runtime — build prerequisite, once per clone
```

**First-time setup, three steps:**

1. Drop three things into the `dist/` folder at the repository root:
   - The GTA:SA v2.10 installation package, **filename starting with `gtasa`** (e.g. `gtasa_2.10_arm64.apk`; if you have separate 32/64-bit APKs, drop both)
   - The game data `main`/`patch` **`.obb`** files (or the 2 GB+ full APK — drop as-is, no renaming needed)
   - (Optional) an open-source Chinese font with `font` in its filename (`.ttf`/`.ttc`/`.otf`)
2. **Double-click `build_tool.bat`** — the menu auto-detects the environment: it can write `local.properties` for you when the Android SDK is found, and lets you type the PC GTA:SA path once if it can't be detected (remembered afterwards)
3. Pick **4** first to extract the runtime (once only), then pick whatever you want to build; artifacts land in **`build/apk/`** and **`build/gamedata/`**

The two distribution modes:

| Mode | Experience after install | Size | Best for |
|---|---|---|---|
| **Bundled** (recommended) | Install → tap "extract now" on first launch → wait a few minutes → play | lite ~530 MB / full ~1.6 GB | Zero manual steps, least hassle |
| **Plain APK + data zip** (menu 2 + 3) | Install APK + copy zip to phone → Settings → File Verification → Local Import | APK ~60 MB + zip 1.0/2.1 GB | Manual import is acceptable; or updating APK and data independently |

> - Lite = no audio streams (SFX/radio silent); full = complete audio. Both editions share the same package name and signature and can install over each other.
> - **The full edition needs ~2.1 GB extracted plus its own 1.6 GB APK — devices should have ~4 GB free** (lite is about half).
> - `dist/friends.jks` is the test-signing key (auto-generated once) — **do not delete it**, or later builds can't install as updates and require uninstall/reinstall.
> - For development pick debug in menu option 2; experienced developers can also run `./gradlew assembleDebug` directly (point `JAVA_HOME` at JDK 17-21 first, see the table below). macOS / Linux: run the scripts in `tools/packaging/` directly (see `--help`).
> - **Compliance**: the produced APKs/zips contain proprietary Rockstar game data — **do not publish them** (see [NOTICE](NOTICE)).

---

## Resource Preparation Details

For copyright compliance this repository ships only source code and open-source dependencies. You provide:

| Resource | Purpose | How to provide |
|---|---|---|
| **GTA:SA Android v2.10 installation package** | Source of `libGTASA.so` & friends plus the lightweight APK assets (required to build the APK) | Name it starting with `gtasa` and put it in `dist/` (case-insensitive; multiple APKs are processed together), then run `prepare_gtasa.py` once; `--from-adb` also works if the game is already installed on your phone |
| **GTA:SA game data** (obb or 2 GB+ full apk) | Single-player base map, models, animations, textures (required for the data package) | Drop `main`/`patch` `.obb` directly into `dist/` — auto-detected |
| **PC GTA:SA + SA-MP 0.3.7** | `SAMP.img` / `SAMP.ide` / `SAMPCOL.img` and PC TextDraw textures | Auto-detected in common install paths; the menu asks for the path when not found, or pass `--pc-samp` |
| **Open-source Chinese font** (e.g. Noto Sans SC) | Generates `SAMP/fonts/chinese.ttf` for Chinese text rendering | Filename containing `font` in `dist/`; falls back to bundled Roboto if omitted |

**Notes on GTA:SA v2.10:**

- Google Play has since updated to v2.11+, whose low-level memory layout is incompatible — you must use **v2.10**.
- Common patched v2.10 packages carry a Fastman92 injection; `prepare_gtasa.py` strips it automatically and relocates the program header table back to standard (without that fix, x86 emulators crash instantly when entering the game). Official unmodified libraries are unaffected.
- **If your `libGTASA.so` was extracted before 2026-11-23** (before the fix existed), it will crash on emulators — re-run the extraction and rebuild.
- A 64-bit-only APK is fine (modern phones); the script merely notes the missing 32-bit libraries.

Other `prepare_gtasa.py` usage: `--apk` to specify APK paths manually, `--libs-only` to refresh only the native libraries, `--clean` to remove extracted proprietary files.

---

## Build Environment

| Tool / Component | Version requirement | Notes |
|---|---|---|
| **JDK** | **17 – 21** (JDK 21 recommended) | Gradle 8.13 does not support JDK 25+ (`major version 69` error) |
| **Android SDK** | `compileSdk 35` / `targetSdk 35` / `minSdk 24` | Platform 35 and Build-Tools 35 required; after cloning, copy `local.properties.example` to `local.properties` and fill in your SDK path (opening the project in Android Studio generates it automatically) |
| **Android NDK** | **`25.1.8937393`** | Pinned in `app/build.gradle` |
| **CMake** | **`3.22.1`** (or compatible) | Installable via Android Studio SDK Manager |
| **Gradle** | **8.13** (Wrapper included) | No global installation needed |
| **Python 3** | 3.10+ | For the `tools/` scripts; the data compiler needs `numpy` (`pip install numpy`) |

Optional extras:

- **Release signing**: configure `SAMP_STORE_FILE` / `SAMP_STORE_PASSWORD` / `SAMP_KEY_ALIAS` / `SAMP_KEY_PASSWORD` in `local.properties` or environment variables, then `./gradlew assembleRelease` (never commit keystore files).
- **Firebase Crashlytics**: place `app/google-services.json` following `app/google-services.json.example` to enable it automatically; disabled by default.
- **Custom launcher backdrop**: put a landscape `launcher_backdrop.jpg` (`.png`/`.webp` also work) into `app/src/main/res/drawable-nodpi/` and rebuild to replace the default gradient; delete the file to restore. Recommended 2400×1080, keep the subject away from the leftmost 1/10 (covered by the navigation bar).

---

## Manual data import (the phone side of split builds)

1. Copy `build/gamedata/gamedata-lite.zip` (or `-full.zip`) to any directory on the phone (e.g. `Download/`) — no need to touch `Android/data/` manually;
2. Open the app → **设置 (Settings) → 文件验证 (File Verification) → 本地导入 (Local Import)** and pick the zip — the launcher UI is currently Chinese-only; the「缺少游戏数据包」("Missing game data package") dialog shown when joining a server also offers a「一键导入 .zip」button;
3. The launcher streams and extracts it into its private `files/` directory, preserving your existing `settings.json` and server list — completely sidestepping Android 14 directory permission pitfalls.

> The data package can also be built standalone: `python tools/gamedata/build_gamedata.py --pc-samp "<PC GTA:SA dir>"` (optional `--keep-audio` for full audio, `--out` for the output path, `--cjk-font` for the font).

---

## Local Verification & Testing

No device needed — the unified entry point `tests/check.py` runs objective verification and automated tests (see [tests/README.md](tests/README.md)):

```powershell
python tests/check.py            # Daily quick check (compliance, platform contracts, dual-ABI symbols/JNI, APK tooling)
python tests/check.py --full     # Full gate (adds C++ host-side unit tests, Gradle JVM unit tests)
python tests/check.py --release  # Strict pre-release gate (adds official Android Lint)
```

- C++ host-side tests separately (WSL/Linux): `sh tests/native/run.sh`
- JVM unit tests separately: `./gradlew testDebugUnitTest`

---

## Screenshots

![Mods & patch management](images/1.jpg)

![Game settings & custom themes](images/2.jpg)

![Screenshot gallery & in-game footage](images/3.jpg)

---

## FAQ

- **Error `Unsupported class file major version 69`**: your current `JAVA_HOME` is JDK 25+ — switch to JDK 17–21 and rebuild.
- **AML / MonetLoader loading issues on Android 14+**: keep `useLegacyPackaging = true` in `app/build.gradle` and `android:allowNativeHeapPointerTagging="false"` in `AndroidManifest.xml` untouched.
- **Slow first Gradle dependency fetch**: `settings.gradle` already configures mirror fallbacks after the official repositories.
- **`No APK given and no dist/gtasa*.apk found`**: no rule-compliant APK in `dist/` — check location (must sit directly in `dist/`), extension (`.apk`; extract the inner `.apk` from `.xapk`/`.apks` first) and filename prefix (must start with `gtasa`).
- **Instant crash when entering the game on an emulator**: your `libGTASA.so` was extracted with an older version of the script — re-run `prepare_gtasa.py` and rebuild.

---

## License & Disclaimer

The original and modified code of this project is open-sourced under the **GNU General Public License v3.0**; the full license text is in [LICENSE](LICENSE), and the third-party component license list is in [third_party/licenses/THIRD_PARTY_NOTICES.md](third_party/licenses/THIRD_PARTY_NOTICES.md).

This project continues development based on the open-source Alyn SA-MP Mobile architecture and retains the original file attribution notices. This project is not affiliated with Rockstar Games, Take-Two Interactive or the official SA-MP team in any way, and distributes no Rockstar proprietary binaries, game data packages or commercial fonts — see [NOTICE](NOTICE) for details.
