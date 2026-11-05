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
| Fire Emblem: Fortune's Weave | `04007EF01F8AE000` | In&nbsp;game&nbsp;+ | Renders since the TXD lowering; faces fixed; 30 FPS docked as the game requests. FSR 3.1 upscaling runs. Regressed in Build 82: very pixelated and the screen shakes; UI and audio fine (Fil). | — |
| Splatoon Raiders | `0400380020056000` | Playable | Fully playable (Fil, 2026-11-04); perfect in Build 82 (Fil). | — |
| Mario Kart World | `0400C3F00006E000` | In&nbsp;game&nbsp;+ | No crashes at all (Fil, Build 82). 30 FPS and below. One graphical error: a blue circle while drifting. | [Details](games/mario-kart-world.md) |
| Nintendo Switch Sports Resort | `04008E001F098000` | Playable | Plays fine, graphics correct, bowling pins back (Fil, 2026-11-04); great in Build 82 (Fil). | [Details](games/switch-sports-resort.md) |
| Yoshi | `0400245021D2C000` | Menus | Regressed in Build 82: UI elements missing again; boots, gameplay is black, then it breaks (Fil). Build 79 had perfect menus with all UI elements and crashed when the gameplay scene loaded. | [Details](games/yoshi.md) |
| Cyberpunk 2077 | `0400DDB020648000` | Boots | Green screen at 0 FPS in Build 82 (Fil); its Bink videos decode to all-zero images. Build 81 added the 16-bit global loads its compute needed, root tables 1-7 for its vertex shaders and three more compute forms. Texture handles read from memory are next. | [Details](games/cyberpunk-2077.md) |
| Kirby Air Riders | `040077201BE50000` | Boots | Fully playable at 60 FPS, if you can imagine seeing the game: it runs behind a black screen. Unchanged in Build 82 (Fil). | — |
| The Legend of Zelda: Ocarina of Time | `0400486024DEC000` | In&nbsp;game | Menus and cutscenes perfect; gameplay runs with very strong bloom (Fil, Build 82). Build 82 added point and line mesh shaders, which failed in a Lichii OoT log. | — |
| Super Mario Wonder | `010015100B514000` | Playable | Fully playable (Fil, 2026-11-04). Capped at 67 Hz (`EDEN_NX2_VSYNC_HZ_010015100B514000=67`). Runs as the Switch 1 version. The dump holds only Switch 1 programs (base and update 327680; the update offers to sell the upgrade) and the upgrade DLC's data, not the Switch 2 Edition program, so Meetup in Bellabel Park does not appear. | — |

To report a result, see [CONTRIBUTING.md](../CONTRIBUTING.md).
