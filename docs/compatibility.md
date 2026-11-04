# Compatibility

Switch 2 titles tested with Satsuma. Each row is the latest result from an actual play
test; "needs testing" marks a fix that is in the build but not yet tried in the game.
Results depend on the GPU, driver and settings. They were recorded on Windows with an
NVIDIA GPU and the Vulkan renderer.

| Status | Meaning |
| --- | --- |
| Playable | Can be played start to finish with minor issues |
| In game | Gets into gameplay, with issues that block or spoil play |
| Menus | Reaches menus or title screen only |
| Boots | Starts, but does not reach menus |
| Not recorded | Tested, but how far it gets has not been written down yet |

| Game | Title ID | Status | Notes | Details |
| --- | --- | --- | --- | --- |
| Fire Emblem: Fortune's Weave | `04007EF01F8AE000` | In game | Renders since the TXD lowering; faces fixed; 30 FPS docked as the game requests. FSR 3.1 upscaling runs. A white-3D gameplay fix (FSET with uniform operand) needs testing. | — |
| Splatoon Raiders | `0400380020056000` | Playable | Fully playable (Fil, 2026-11-04). | — |
| Mario Kart World | `0400C3F00006E000` | In game | Performance much better. Crashes consistently: the command stream desyncs (unbound subchannel, bad syncpoint IDs, then garbage GPU state); also seen on 2026-11-03 at 169 s. | — |
| Nintendo Switch Sports Resort | `04008E001F098000` | Playable | Plays fine, graphics correct, bowling pins back (Fil, 2026-11-04). Joy-Con 2 limits: stand-in only, and the prompt asks again after OK. Performance good after the intro. | [Details](games/switch-sports-resort.md) |
| Yoshi | `0400245021D2C000` | In game | Reaches gameplay again; gameplay renders black and many UI elements are missing. Gameplay shaders still read as zeros. | [Details](games/yoshi.md) |
| Cyberpunk 2077 | `0400DDB020648000` | Boots | Launches to a green screen. Five rejected compute shaders now lower; needs testing. | [Details](games/cyberpunk-2077.md) |
| Kirby Air Riders | `040077201BE50000` | Boots | Launches since the built-in font fix (was aborting with 2083-0419); black screen. | — |
| Super Mario Bros. Wonder - Nintendo Switch 2 Edition + Meetup in Bellabel Park | `010015100B514000` | Playable | Fully playable (Fil, 2026-11-04). Runs at 60 Hz (`EDEN_NX2_VSYNC_HZ_010015100B514000=60`). The Switch 2 Edition DLC installed to NAND is not picked up yet: the game never asks the add-on content service for its list. | — |

To report a result, see [CONTRIBUTING.md](../CONTRIBUTING.md).
