# arm64-bridge-bl2

An ARM32 → ARM64 bridge that runs the original Android build of **Borderlands 2**
on 64-bit-only Android devices, plus a native control layer: touch, gamepad,
mouse and keyboard.

The Android version of the game (`com.tt2kgames.Borderlands2`, 1.0.0.0.33) ships
a 32-bit ARM Unreal Engine 3 library, and more and more phones cannot run 32-bit
code at all. The bridge is a 64-bit `libUnrealEngine3.so` that takes the place of
the original library. It loads the original ARM32 `libUnrealEngine3.so`,
translates it with a patched [Dynarmic](https://github.com/azahar-emu/dynarmic)
JIT, and provides host implementations of the libc, EGL/OpenGL ES, OpenSL ES, JNI
and Android APIs the engine calls.

> **No game files are included.** This repository holds only the source code of
> the bridge and the controls: no APK or OBB, no game binaries, no decompiled
> code, no resources or assets, and nothing generated from them. You need your
> own copy of the game.

## Features

**Runtime**

- Runs the original ARM32 engine on arm64-v8a devices with Android 7.0+
  (API 24) and OpenGL ES 3.2; the library is 16 KB page-size compatible.
- Dynarmic patches for this workload: fast and inline dispatch caches with a
  16-entry return stack, fastmem FPSR/FPCR handling, guest write watch,
  conditional-branch fusion, relaxed halt polling, callback-time precompilation
  and a learned JIT warmup.
- OpenGL ES calls run on a dedicated worker thread. Shader programs are formed
  in the background, with progress shown in the main menu, and cached across
  launches.
- The engine's periodic garbage collection moves out of gameplay into menus.
- Native replacements for hot engine routines (visibility culling, the audio
  limiter and others). Each one checks the original code before it is
  installed; the culling and audio-limiter ones have switches in the graphics
  settings.
- OBB data is read in place from installed split APKs or from `Android/obb`,
  without extraction.
- A local platform layer replaces the Google Play and NVIDIA services: local
  profile, saves and achievement events; LAN co-op is restored.
- Render scale 20–150 %, UI scale 100–200 %, an FPS limit (30/60/90/display
  rate), extra graphics options and a "potato" mode.
- Skips the static startup screens; a tap skips the startup movies.
- Routes Russian text and voice packages into the game (the packages are not
  part of this repository).
- Optional access to the game's built-in Pause → Debug menu.

**Controls**

- Touch: movement, look, fire, aim, jump, grenade, skill and use, a context
  panel for rarer commands, and a vehicle layout.
- HUD gestures: the minimap opens the map, the ammo bar reloads, a swipe
  switches weapons, a hold opens the four-slot selector.
- Menus: taps, continuous list scrolling, tab swipes, map panning, pinch zoom
  and waypoints by hold, with one-time gesture hints.
- Settings: button size, opacity and a layout editor, vector icons, left-handed
  mode, look and aim sensitivity up to 3×, look acceleration, auto-run, gyro
  aiming and haptics.
- Gamepad navigation of the port's settings pages; Bluetooth and USB mice
  (pointer and wheel in menus, mouse look and buttons in game); a
  keyboard-and-mouse mode that hides the touch controls; the on-screen keyboard
  opens for text fields such as chat.

## Repository layout

| Path | Contents |
|---|---|
| `arm64_bridge/` | The bridge (CMake project): ELF loader, Dynarmic host, thunks, engine hooks, renderer and audio work, touch runtime; `patches/` for Dynarmic; `tests/` |
| `touch_native_design/` | Native touch input core (mailbox and JNI contract), host tests and the scripts that run them |
| `android_port/src/` | Java side: touch overlay, launch and port settings, local platform, saves and storage |
| `work/scripts/` | APK build scripts and device QA tools (virtual HID gamepad, mouse, keyboard, multitouch) |

## Building the bridge

Requirements: Android SDK with NDK 27.2.12479018 and CMake 3.22.1 (Ninja is in
the same SDK package), Git, Python 3, and PowerShell 7 for the scripts. The
commands below are for a POSIX shell; Git Bash works on Windows.

1. Fetch the third-party sources into `arm64_bridge/third_party/` (ignored by
   Git). On Windows, enable long paths first
   (`git config --global core.longpaths true`) and keep the checkout path short.

   ```sh
   git clone https://github.com/azahar-emu/dynarmic.git arm64_bridge/third_party/_deps/dynarmic-src
   git -C arm64_bridge/third_party/_deps/dynarmic-src checkout e77b1ba0b7da7cbe93021b01a663acfe7c4dd516
   git -C arm64_bridge/third_party/_deps/dynarmic-src submodule update --init --recursive

   curl -LO https://archives.boost.io/release/1.83.0/source/boost_1_83_0.tar.gz
   mkdir -p arm64_bridge/third_party/boost
   tar -xzf boost_1_83_0.tar.gz --strip-components=1 -C arm64_bridge/third_party/boost boost_1_83_0/boost
   ```

   The Boost archive's SHA-256 is
   `c0685b68dd44cc46574cce86c4e17c0f611b15e195be9848dfd0769a0a207628`.

2. Configure and build. Configuring applies `arm64_bridge/patches/` to the
   Dynarmic checkout; running it again is safe.

   ```sh
   cmake -S arm64_bridge -B build/arm64 -G Ninja \
     -DCMAKE_TOOLCHAIN_FILE="$ANDROID_HOME/ndk/27.2.12479018/build/cmake/android.toolchain.cmake" \
     -DANDROID_ABI=arm64-v8a -DANDROID_PLATFORM=android-24 -DCMAKE_BUILD_TYPE=Release
   cmake --build build/arm64 --parallel
   ```

   The result is `build/arm64/libUnrealEngine3.so`. It keeps the original
   library's name, so the game's Java side loads the bridge, and the bridge
   then loads the original ARM32 library from the APK's
   `assets/guest_libs/libUnrealEngine3.so`.

| CMake option | Default | Effect |
|---|---|---|
| `HORN_ENABLE_LOGGING` | `OFF` | Bridge and forwarded guest logcat output |
| `HORN_ENABLE_PERF_PROBE` | `OFF` | On-device performance diagnostics |
| `HORN_ENABLE_PHYSX_COOKIE_WATCH` | `OFF` | PhysX first-write diagnostic (needs logging) |
| `HORN_GL_SELF_CHECK` | `OFF` | Bounded GLES thunk self-check |
| `BL2_ENABLE_DEBUG_MENU_DEFAULT` | `OFF` | Show Pause → Debug by default |
| `HORN_SILENT_LOGCAT` | `OFF` | Final build without any logcat output |

## Tests and QA tools

- `touch_native_design/tools/check_*.ps1` build and run the control tests on
  Windows. Run them with PowerShell 7 from a Developer PowerShell for Visual
  Studio (they use `VCToolsInstallDir`, `WindowsSdkDir` and `WindowsSDKVersion`)
  with `ANDROID_HOME` set. `check_java.ps1` compiles the Java sources against
  `platforms/android-36`, runs `TouchInputTest` and checks the result with D8
  from `build-tools/37.0.0`; it needs `JAVA_HOME`.
- `check_gfx_values.ps1`, `check_widget_commands.ps1`,
  `check_widget_router.ps1`, `test_footer_glyphs.ps1` and the `verify_*.py`
  scripts also check the original ARM32 library. They expect your copy at
  `app/assets/guest_libs/libUnrealEngine3.so`.
- The bridge self-tests are `EXCLUDE_FROM_ALL` targets of
  `arm64_bridge/CMakeLists.txt` (for example `bl2_fastmem_fpsr_test` or
  `bl2_software_write_watch_test`). Build one with
  `cmake --build build/arm64 --target <name>`, push it to `/data/local/tmp`
  and run it with `adb shell`.
- `work/scripts/bl2_virtual_{gamepad,mouse,kbm,multitouch}.py` register a
  virtual HID gamepad, mouse, keyboard + mouse or two-finger touchscreen on a
  device through `/dev/uhid` and Android's `hid` tool (no root), then play a
  list of steps, for example
  `python -I work/scripts/bl2_virtual_mouse.py --serial SERIAL home to:600,300 wait:300 click:left`.
  `invoke_*.ps1` send adb input to measured on-screen controls.

## Assembling an APK

`work/scripts/build_bridge_apk.ps1 -Seed <n> [-Release | -Final | -Diagnostics]`
builds the bridge, compiles the Java layer (`build_launch_controls.ps1`) and
repackages an apktool-decoded APK in `app/`. That directory is not part of this
repository: it holds your copy of the game, the port's integration in its
decompiled Java layer and data tables generated from the game, none of which can
be published. The script reads `ANDROID_HOME` (CMake 3.22.1, NDK 27.2.12479018,
build-tools 37.0.0, platforms/android-36), `JAVA_HOME`, `APKTOOL_JAR`,
`BAKSMALI_JAR` and `APK_SIGNER_DIR` (`apksigner.jar`, `testkey.pk8`,
`testkey.x509.pem`), and needs Python 3 on `PATH`.

## License

[GNU General Public License v3.0 or later](LICENSE). Any distributed work based
on this code must be released under the same license, with its source.

Third-party code fetched for the build keeps its own license: Dynarmic (0BSD)
and the libraries bundled with it, Boost (Boost Software License 1.0).

## Disclaimer

This is an unofficial project, not affiliated with or endorsed by Gearbox
Software, 2K, Take-Two Interactive or NVIDIA. Borderlands is a trademark of
Gearbox Software, LLC. The sources refer to the supported game build by symbol
names, addresses, short instruction signatures and SHA-256 hashes; the bridge
uses them to verify the original library before it patches anything. The
supported library is `libUnrealEngine3.so` with SHA-256
`A0B06EF59F4C89C0889654196951D2DEB966CFEAC7180699B5724F0E26938814`.

## Кратко по-русски

ARM32 → ARM64-мост, который запускает оригинальную Android-версию Borderlands 2
(1.0.0.0.33) на устройствах без поддержки 32-битного кода, и нативное управление:
сенсорное, геймпад, мышь и клавиатура. Мост транслирует исходный ARM32-движок
через пропатченный Dynarmic и подменяет системные вызовы, OpenGL ES, OpenSL ES и
JNI. В репозитории только код моста и управления, файлов игры нет — нужна
собственная копия игры. Сборка и тесты описаны выше. Лицензия — GPL-3.0-or-later.
