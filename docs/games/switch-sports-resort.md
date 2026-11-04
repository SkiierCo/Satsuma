# Nintendo Switch Sports Resort

Program ID: `04008E001F098000`. Status: playable (see [compatibility](../compatibility.md)).

## Symptoms

- Starting a game mode froze, with music still playing and some UI missing. Fixed.
- Water is missing and some UI elements are missing.
- The Joy-Con 2 prompt is answered by a stand-in: while the game declares Joy-Con 2
  support, the connected controller is reported as one. Real Joy-Con 2 cannot connect to
  a PC; input through the stand-in still needs a full check.

## Findings

**Game-mode freeze** was a crash. The game aborted (2162-0001) 6 ms after
`GetSixAxisSensorIcInformation`, which answered all zeros. It now reports a typical sensor
(±2000 °/s, ±8 g, identity calibration). Confirmed: game modes start.

**Skipped draws.** In the game-mode logs every skipped draw came from one of two pipeline
errors:

- *Failed to track indirect branch* (53 full-screen draws, likely the water and most
  missing UI). The lowered programs clamp their jump-table index with a SEL, and the
  shader translator only accepted an IMNMX bound. Fixed: lowered programs' tables are read
  while their entries are valid code addresses. Offline, all 18 jump tables and all 33
  programs from the failed pipelines now translate (`tests.exe "[.brxprobe]"` over the
  dumped programs).
- *Mesh output topology 0* (640×1440 and 192×90 layers). These are real mesh pipelines
  (their mesh program has its own constant buffers), yet the mesh registers read zero when
  they are translated. Unresolved; the build logs the mesh registers and the methods
  around them when it happens.

**Texture handles.** Shader compilation asserts that some texture handles are past the end
of the texture table (over 1,000 times) and falls back to a 2D guess. Not investigated yet.

## 2026-11-04 test

Water and all UI render. The fixes reached the game once the shader-reuse wrapper kept mesh
state and native queries. The menu stutters while new shaders compile (about 5 ms each on
the GPU thread). Pins are missing, with no skipped draw in the log. The Joy-Con 2 stand-in
only acts while the game declares Joy-Con 2 (style 0x1002); the game later switches to 0x6.

## 2026-11-04 test (later build)

Playable (Fil): game modes play fine; performance is good after the intro. Joy-Con 2 works
only through the stand-in. Rendering is off, and the water looks wrong again.

The texture handles past the pool are one value, `0x96c6b000` (texture field `0x6b000`,
sampler `0x96c`; pool limit 33067), read as byte offset 13696. No tracked handle was shifted
by an AND mask, so the value comes from the game's own data, not from Satsuma's handle
tracker. Switch Sports' NVN driver builds handles as `texture | sampler << 20`; what decides
the texture field is how the driver registers textures in the pool, not checked yet.

## Next

Test the 2026-11-04 build: water, UI, a game mode. Send the log; any "Mesh program" lines
explain the mesh layers.
