# Changes

Changes since the Satsuma v1 source snapshot, newest first. Each entry names the game it
was found in; "needs testing" means it builds and passes the tests but has not been tried
in that game yet. The commit history has the full detail.

## 2026-11-04

### Audio

- **Switch 2 hardware audio codec (hwacod).** `hwopus` command 100 now returns a working
  codec session: shared-memory task and completion queues driving a batch Opus decoder.
  It used to be a stub that only logged, and Cyberpunk 2077 rejected its replies. Ported
  from Lichii build 70 with its tests. Needs testing in Cyberpunk 2077.

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
  Needs testing in Yoshi.
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
