<!--
# SPDX-FileCopyrightText: Copyright 2025 Eden Emulator Project
# SPDX-License-Identifier: GPL-3.0-or-later

# SPDX-FileCopyrightText: 2018 yuzu Emulator Project
# SPDX-License-Identifier: GPL-2.0-or-later
-->
<!-- lang: en-GB -->

<h1 align="center">
  <img src="dist/satsuma.png" alt="Satsuma" width="200">
  <br>
  <b>Satsuma</b>
</h1>

<h4 align="center">An open-source Nintendo Switch 2 emulator for Windows, written in C++.</h4>

<p align="center">
  <a href="#status">Status</a> |
  <a href="#building">Building</a> |
  <a href="#credits">Credits</a> |
  <a href="#license">License</a>
</p>

## Status

Satsuma is in active development. Switch 2 titles boot and several are playable;
others still render incorrectly or have missing audio. Known issues are tracked
per game as they are found.

Satsuma does not include keys, firmware or games. You need to dump them from your
own console.

## Building

Satsuma builds on Windows with Visual Studio 2026 (x64 C++ tools), CMake and Ninja.
See [BUILD-SATSUMA.md](BUILD-SATSUMA.md) for the exact commands. The general
[build guide](docs/Build.md) covers dependencies and other platforms.

Settings, saves and caches live in `%APPDATA%\satsuma`. A `user` folder next to
the executable switches to portable mode.

## Credits

Satsuma is derived from [Eglantine](https://github.com/Djihads80/Eglantine),
itself based on [Eden](https://git.eden-emu.dev/eden-emu/eden), yuzu and Sudachi.
Their copyright notices are kept in the source.

## License

Satsuma is licensed under the GPLv3 or any later version. See
[LICENSE.txt](LICENSE.txt). Some components are under other licences, listed in
[LICENSES](LICENSES).
