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

<h4 align="center">An open-source Nintendo Switch 2 emulator for Windows.</h4>

## Status

Satsuma is in active development. Switch 2 titles boot and multiple games are
playable from start to finish; others still have rendering issues or straight up
unplayable but I'm working through multiple games at a time.

See the [compatibility list](docs/compatibility.md) for each game's status and the
[change log](docs/changelog.md) for what changed.

Satsuma does not include keys, firmware or games. You need to dump them from your
own console.

## Download

Builds are on the [releases page](https://github.com/SkiierCo/Satsuma/releases). Extract the zip and run
`satsuma.exe`; the `user` folder next to it keeps settings and saves.

## Switch 2 Editions of Switch 1 games

There is no separate Switch 2 Edition program in releases of Switch 1 games labelled
"Nintendo Switch 2 Edition": they contain the Switch 1 game. In Satsuma they launch as the
Switch 1 version, the game doesn't say Switch 2 Edition, and Switch 2 Edition DLC (such as
Super Mario Bros. Wonder's Meetup in Bellabel Park) can't be used.

## Building

Satsuma builds on Windows with Visual Studio 2026 (x64 C++ tools), CMake and Ninja.
See [BUILD-SATSUMA.md](BUILD-SATSUMA.md) for the exact commands. The general
[build guide](docs/Build.md) covers dependencies and other platforms.

Settings, saves and caches live in `%APPDATA%\satsuma`. A `user` folder next to
the executable switches to portable mode.

## Credits

Satsuma is derived from [Eglantine](https://github.com/Djihads80/Eglantine), Updated and
maintained by Fil, Some patches and additions are thanks to efforts by Zortos, Selevet,
and UesleiDev (creator of Nemyu), respectively.

## License

Satsuma is licensed under the GPLv3 or any later version. See
[LICENSE.txt](LICENSE.txt). Some components are under other licences, listed in
[LICENSES](LICENSES).
