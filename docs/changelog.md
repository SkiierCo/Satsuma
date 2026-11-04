# Changes

Changes since the Satsuma v1 source snapshot, newest first. Each entry names the game it
was found in; "needs testing" means it builds and passes the tests but has not been tried
in that game yet. The commit history has the full detail.

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
