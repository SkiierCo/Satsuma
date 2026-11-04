# Changes

Changes since the Satsuma v1 source snapshot, newest first. Each entry names the game it
was found in; "needs testing" means it builds and passes the tests but has not been tried
in that game yet. The commit history has the full detail.

## 2026-11-04 (third round)

- **Where byte-offset texture handles come from.** Switch Sports' own NVN driver code builds
  handles as `texture | sampler << 20` from pool indices, never byte offsets, so the
  `index x 32` handles the byte-offset rule catches are likely made by the emulator. One
  suspect: the handle tracker reads `value & mask` as `value` shifted left by the mask's
  trailing zeros. The log now shows each tracked handle that was shifted this way and each
  handle the byte-offset rule changed, to confirm the source in Switch Sports.
- **Splatoon Raiders: fully playable** (Fil).
- **Blit dither bias as on the console.** Native fragment shaders read words 0x10-0x18 of the
  blit constant block as an integer dither bias, which the console leaves zero. Satsuma filled
  them with the texel size and UV scale, and patched only Kirby Air Riders' SMAA resolve by
  removing its add. Fragment shaders now read zero there, as on the console; vertex shaders
  keep the texel size and UV scale. Shaders already in the pipeline cache keep their old
  translation. Needs testing in Kirby.
- **12 GiB of console memory, as on the Switch 2.** The 12 GB layout used to add an extra
  8 GiB pool (20 GiB in all) for the graphics server's own memory (`nx2:gpu` 214 GPU-only
  and sparse regions). That memory now comes from the system pool inside the 12 GiB, as on
  the console; games are still told 9.58 GiB. If a region doesn't fit, the log says
  `nx2:gpu 214 could not allocate`. Needs testing, Mario Kart World first (six 300 MB
  GPU-only regions).
- **Only Eglantine, zortos (Lichii) and Satsuma code.** Implementations taken from other
  sources are removed:
  - NVIDIA DLSS through NVIDIA Streamline (the "Use NVIDIA DLSS in place of a game's DLSS"
    setting, its `streamline` user folder and the Streamline headers). FSR 3.1 and the
    temporal stand-in, from Eglantine, stay; `EDEN_NX2_DLSS_REPLACEMENT` still picks between
    them.
  - The once-a-frame re-upload of 64 KiB constant banks (PokeDinner fork;
    `EDEN_NX2_BANK_REFRESH`, already off in the shipped switches, now does nothing).
  - The texture descriptor table from nemyu. It is now Lichii's, with Satsuma's own cap on the
    table size so a desynced command stream can't make it allocate hundreds of GiB.
    Lichii's table reserves about 16 MiB per table up front.
  Code from the PokeDinner fork that Lichii also has (GPU-written command list copies, the
  `0x2bf`, `0x306`, `0x593`, `0x8a5` and `0xf63` lowering) stays. Needs testing.
- **Cyberpunk 2077 compute shaders.** Five programs were rejected at load. Added: texel
  fetches through a uniform-register handle (`0xf66`) or a register handle (`0x367`), 1D,
  2D, 3D and 1D/2D arrays, level zero or a given level; register-handle surface loads and
  stores on 1D buffers (`0x99c`). Built from Cyberpunk's own instructions, with tests.
  Texel offsets and multisample forms of these fetches are not seen in any captured game
  and stay rejected. Needs testing for the green screen.
- **Mapping history in the cache probe.** When a shader or command list reads as zeros or
  garbage, the log now lists the last GPU and device mapping operations at that address
  (Mario Kart World's crash, Yoshi's zeroed shaders).

## 2026-11-04 (second round)

- **Shader reuse lost mesh and native state.** Translation goes through a recording wrapper
  (shader reuse) that copied the stage but not the mesh registers, and did not forward the
  native-program queries. Mesh programs translated with `SET_MESH_SHADER_A` zero, and the
  jump-table fix below never ran in the game. The wrapper now copies the mesh state, forwards
  the native queries, records storage-pointer answers, and won't reuse a mesh pipeline under
  different mesh settings. Tested: Switch Sports' water and UI are fixed.
- **Shader cache flush off by default.** Copying GPU-written pages back before reading
  shaders broke Yoshi again, and Yoshi's zeroed code turned out to be in no cache at all.
  It is now opt-in (`EDEN_NX2_SHADER_FLUSH=1`).
- **Indirect-branch errors** now report the table's location, first entries and the
  program's size.
- **Kirby Air Riders, from Lichii build 70.** Needs testing in Kirby.
  - White world: the SMAA resolve adds a dither bias from bank 17 to its output bits; the
    console leaves it zero, the blit constants fill it, and every pixel saturated to white.
    The bias is now dropped, also when the colour comes through a phi.
  - Geometry programs whose subroutines return before the exit handler now lower.
  - Guest registers written by FMUL (`0x220`) and other opcodes are preserved around the
    uniform-register mirrors.
  - `IGraphicsScaleController` command 1050 is handled; command 10's reply is complete.
- **Switch 2 built-in fonts.** `GetBuiltInFontData` answered size 0 once the Switch 1 font
  memory was full, and Kirby Air Riders aborted (2083-0419) asking for it. The plain fonts now
  get their own shared memory. From Lichii build 70. Needs testing in Kirby.
- **Joy-Con 2 stand-in in the controller prompt.** The controller applet built its list and
  its checks from the game's styles only (Switch Sports: handheld + Joy-Con 2), so dual
  Joy-Cons were missing and the prompt switched to handheld. The stand-in types (Pro
  Controller, dual, left and right Joy-Cons) are now listed and accepted there too.
  Needs testing.
- **Joy-Con 2 stand-in in handheld.** Switch Sports asks for handheld + Joy-Con 2; with a
  handheld controller it saw a Switch 1 Joy-Con and recommended switching. Handheld now
  stands in too. Needs testing.
- **DMA scatter switch.** `EDEN_NX2_DMA_SCATTER=0` restores the old single-run DMA copy, to
  check whether Mario Kart World's command-stream desync became more frequent with it.
- **Global atomics and storage-buffer tracking**, from Lichii build 70. Global atomic adds
  that stay global (Donkey Kong Bananza's append counters, Kirby's compute) are resolved
  among the bound storage buffers instead of failing, and pointers carried through a 64-bit
  add with carry are recognised. Lichii's rewritten storage-buffer tracker is merged with
  Satsuma's pruned search (fast translation of very large shaders kept); NVN descriptor slots
  are matched by offset; storage buffers are marked coherent when the program needs it.
  With Lichii's 23 storage-buffer tests. Affects every game's storage buffers.
- **Texture handles holding a byte offset.** Some Switch 2 bindless handles (Switch Sports'
  scenery) hold the texture header's byte offset (index × 32) instead of its index, and
  pointed past the texture table. On Switch 2 they are read as an offset when the index is
  out of range and the offset is inside. From Lichii build 70. Needs testing in Switch Sports.
- **Register-handle surface loads and stores** (`0x998`, `0x99c`; The Duskbloods), from
  Lichii build 70, with its tests.

## 2026-11-04

### Audio

- **Switch 2 hardware audio codec (hwacod).** `hwopus` command 100 now returns a working
  codec session: shared-memory task and completion queues driving a batch Opus decoder.
  It used to be a stub that only logged, and Cyberpunk 2077 rejected its replies. Ported
  from Lichii build 70 with its tests. Tested: Cyberpunk 2077 now launches, to a green
  screen.

### Graphics

- **Jump tables in lowered shaders.** Switch Sports clamps jump-table indices with a SEL
  instead of IMNMX, so the shader translator could not find the table and 53 full-screen
  draws were skipped. Tables in lowered programs are now read while their entries are
  valid code addresses. All of Sports' failed programs now translate offline. Needs
  testing for its water and missing UI.
- **Mesh pipelines without mesh settings.** Draws with mesh control set but
  `SET_MESH_SHADER_A` zero are drawn as ordinary pipelines, and the mesh registers are
  logged when a mesh program still sees zero (Switch Sports UI layers; under investigation).
- **Shader code the GPU wrote.** Yoshi copies its shader code with the DMA engine. The
  copy and clear paths assumed each range was one unbroken run of memory and wrote the
  wrong pages; they now go page by page. Pages of a shader that only the buffer cache holds
  are copied back before the shader is read (`EDEN_NX2_SHADER_FLUSH=0` turns this off).
  Tested: Yoshi broke again; the flush is now off by default (see above).
- **Shader instructions.** Added: uniform-handle texture gathers on 2D arrays, cube maps
  and with texel offsets (`0xf63`); global atomics with any 32-bit operation (`0x3a8`,
  `0x9a8`); surface reductions in every dimension (`0x59f`); integer-to-float with an
  immediate (`0x845`). Yoshi's shader dump: 1504 → 1508 of 1594 programs lower.
- **Cache probe.** When a shader reads as zeros or overruns its capture, the log lists, per
  page, which GPU cache holds it.

### Input

- **Motion sensor information.** `GetSixAxisSensorIcInformation` returned all zeros, and
  Switch Sports aborted (2162-0001) when starting a game mode. It now reports a typical
  sensor (±2000 °/s, ±8 g). Confirmed: game modes start.

### Services

- **Streamplay guest input memory.** `stpl:u` command 300 returns its shared memory
  instead of an event. Star Fox crashed on the event. Ported from Lichii build 70.

### Presentation and UI

- **Present pacing.** A changed swap interval applies to the present that changes it,
  not one present later. Ported from Lichii build 70.
- **Window repaint.** Moving the window or switching to or from borderless fullscreen
  repaints the UI and rebuilds the swapchain, which left widgets duplicated or missing
  until the next resize. Confirmed fixed.
