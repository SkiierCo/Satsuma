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
| Splatoon Raiders | `0400380020056000` | Not recorded | Docked washout fixed by software tensor emulation (default for Switch 2 titles). Frame rate drops under load. | — |
| Mario Kart World | `0400C3F00006E000` | In game | Performance much better. Crashes consistently: the command stream desyncs (unbound subchannel, bad syncpoint IDs, then garbage GPU state); also seen on 2026-11-03 at 169 s. | — |
| Nintendo Switch Sports Resort | `04008E001F098000` | In game | Water and UI fixed. Menu stutters while new shaders compile. Pins missing; the Joy-Con 2 stand-in is not taking effect. | [Details](games/switch-sports-resort.md) |
| Yoshi | `0400245021D2C000` | Menus | Menus and the opening cutscene work. The game crashes (2162-0001) during the white fade into gameplay; gameplay shaders still read as zeros. | [Details](games/yoshi.md) |
| Cyberpunk 2077 | `0400DDB020648000` | Boots | Launches to a green screen since the hardware audio codec port. | [Details](games/cyberpunk-2077.md) |
| Kirby Air Riders | `040077201BE50000` | Not recorded | Lichii's white-world, geometry-return and register fixes are ported; needs testing. | — |

To report a result, see [CONTRIBUTING.md](../CONTRIBUTING.md).
