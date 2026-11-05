# Building Satsuma v1 on Windows

This source snapshot corresponds to Satsuma v1. The application target is still
named `yuzu` in CMake; its output is `satsuma.exe` and `satsuma.pdb`.

Use Visual Studio 2026 x64 C++ tools, CMake and Ninja. Dependency declarations and
pins are in the included CMake files and `externals`. The verified build used
MSVC 19.51.36256.0 and the pinned Qt 6.11.1 static dependencies.

From a Visual Studio x64 developer shell:

```powershell
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DENABLE_WEB_SERVICE=OFF -DENABLE_UPDATE_CHECKER=OFF -DENABLE_QT_TRANSLATION=OFF -DUSE_DISCORD_PRESENCE=OFF
cmake --build build --target yuzu tests --parallel 8
```

The build number is in `dist/build_number.txt` (one number). Raise it for every new
build; it shows in the title bar, the About box and the log as "Build <number>".

Windows uses `%APPDATA%\satsuma` for settings, saves and caches. A `user` folder
next to the executable still explicitly enables portable mode. The released
package does not include a portable profile, so installed and packaged copies
use the same Satsuma data directory.

The default CPU Clock setting is Normal. The supplied `nx2_switches.txt` selects
a 120 Hz display target; the normal launcher honors that file. A display target
does not guarantee that every game can render 120 frames per second under load.

Original copyright and licence notices are retained. No personal profile, keys,
firmware, games, save files or captured game shaders are part of this source.
