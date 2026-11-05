# Compatibility

Switch 2 titles tested with Satsuma. Each row is the latest result from an actual play
test; "needs testing" marks a fix that is in the build but not yet tried in the game.
Results depend on the GPU, driver and settings. They were recorded on Windows with an
NVIDIA GPU and the Vulkan renderer.

Releases of Switch 1 games labelled "Nintendo Switch 2 Edition" contain only the Switch 1
game. They launch as the Switch 1 version, without Switch 2 Edition in the game, and their
Switch 2 Edition DLC can't be used.

| Status | Meaning |
| --- | --- |
| Playable | Can be played start to finish with minor issues |
| In&nbsp;game&nbsp;+ | Plays well, but finishing it start to finish and steady performance are not assured |
| In game | Gets into gameplay, with issues that block or spoil play |
| Menus | Reaches menus or title screen only |
| Boots | Starts, but does not reach menus |
| Not recorded | Tested, but how far it gets has not been written down yet |

| Game | Title ID | Status | Notes | Details |
| --- | --- | --- | --- | --- |
| Fire Emblem: Fortune's Weave | `04007EF01F8AE000` | In&nbsp;game&nbsp;+ | Renders since the TXD lowering; faces fixed; 30 FPS docked as the game requests. FSR 3.1 upscaling runs. | — |
| Splatoon Raiders | `0400380020056000` | Playable | Fully playable (Fil, 2026-11-04). | — |
| Mario Kart World | `0400C3F00006E000` | In&nbsp;game&nbsp;+ | No longer crashes: over 21 minutes without a crash since the command-stream desync fix (Fil, Build 74). Graphics correct; 30 FPS. | [Details](games/mario-kart-world.md) |
| Nintendo Switch Sports Resort | `04008E001F098000` | Playable | Plays fine, graphics correct, bowling pins back (Fil, 2026-11-04). | [Details](games/switch-sports-resort.md) |
| Yoshi | `0400245021D2C000` | Menus | Perfect menus with all UI elements; goes from the menu to the cutscene, then crashes when the gameplay scene loads (Fil, Build 79). | [Details](games/yoshi.md) |
| Cyberpunk 2077 | `0400DDB020648000` | Boots | Launches to a green screen (its Bink videos decode to all-zero images). Every dumped shader now translates and the compute queue gets further; a missing SPIR-V instruction and texture handles read from memory are next. | [Details](games/cyberpunk-2077.md) |
| Kirby Air Riders | `040077201BE50000` | Boots | Fully playable at 60 FPS, if you can imagine seeing the game: it runs behind a black screen. | — |
| The Legend of Zelda: Ocarina of Time | `0400486024DEC000` | In&nbsp;game | Intro movie plays cleanly (since the AV1 support in Build 78) and gameplay runs, but 3D scenes are washed out white (Fil). | — |
| Super Mario Wonder | `010015100B514000` | Playable | Fully playable (Fil, 2026-11-04). Capped at 67 Hz (`EDEN_NX2_VSYNC_HZ_010015100B514000=67`). Runs as the Switch 1 version. The dump holds only Switch 1 programs (base and update 327680; the update offers to sell the upgrade) and the upgrade DLC's data, not the Switch 2 Edition program, so Meetup in Bellabel Park does not appear. | — |

To report a result, see [CONTRIBUTING.md](../CONTRIBUTING.md).
