# Changes

Changes since the Satsuma v1 source snapshot, newest first. Each entry names the game it
was found in; "needs testing" means it builds and passes the tests but has not been tried
in that game yet. The commit history has the full detail.

## 2026-11-05

- **Mario Kart World: 30 FPS, the split threads take turns (Build 76).** The game asks for 60 FPS
  and Satsuma accepts 60 presents a second, but the host finishes about 30 frames. Fil's profile
  run: each second the split's front thread waited about 450 ms for its back thread (some 8,000
  times) and the back waited 180-330 ms for the front, so the two mostly took turns. Build 76:
  - The profiler now logs where the front waits for the back ("waits for the back:" with time and
    count per site: ring full, list writers, list flush, macro parameters, macro DMA read, puller,
    GP sync fence, CPU read, map, front start).
  - Macro parameters continued in an entry of their own (Mario Kart World does this) are flushed
    from the caches only when their pages were written by GPU work; before, the front waited for
    the back at every such entry. `EDEN_NX2_PBFLUSH=2` restores that.
  Tested (Fil, profile run): still 30 FPS. The front still waits 340-500 ms a second: puller
  (semaphores, fences) 150-210 ms at 2,200-3,500 waits, macro parameters 133-214 ms at
  1,000-1,900 (these pages really are GPU-written), macro DMA reads 45-75 ms.
- **Mario Kart World: the blue circle around the drift sparks.** From Fil's RenderDoc capture: it
  is the game's glow sprite (a 128x128 radial falloff) drawn additively at each exhaust, with its
  soft-particle depth fade working as the shader intends (fade 0.27, alpha 0.16 at the halo).
  Its colour is multiplied by about 4 from the game's per-frame constants (3.999, which looks
  like a ceiling). Not fixed yet; the next lead is where the game gets that value (possibly an
  exposure value it reads back from the GPU).
- **Frame captures replay again (Build 75).** RenderDoc couldn't replay Fil's Mario Kart World
  captures (`VK_ERROR_INVALID_OPAQUE_CAPTURE_ADDRESS`): Satsuma's descriptor buffers need buffer
  device addresses, which RenderDoc fails to restore on NVIDIA. `EDEN_NX2_DESCRIPTOR_BUFFER=0`
  uses descriptor sets instead; "Capture Mario Kart drift.cmd" sets it, skips RenderDoc's API
  validation (these frames are ~19 GB before compression) and moves the frames into its capture
  folder.
- **Mario Kart World: command-stream desync, the stale device ranges (Build 74).** Fil's
  Build 73 run: the terrain started glitching, then the game crashed at 121 s. The mapping
  history in that desync report shows the cause: the game created GPU memory at `0x11be802000`
  (0x556000 bytes), destroyed it, created and destroyed a larger one at the same address, then
  mapped a 2-page memory there, and Satsuma reused the device (SMMU) range of the first one for
  it. Destroyed memory never gave up its device ranges, and a device range keeps the pages its
  memory had when it was mapped, so the new memory showed the old pages: the command list at
  `0x11bea8cc9c` read as vertex data. Destroying GPU memory (`nx2:gpu` 102) now releases its
  device ranges once no GPU mapping uses it, and a device range is reused only while it still
  reaches the memory the CPU address has. Tested (Fil): no crash in over 21 minutes (the longest
  run before crashed at 689 s), no desync report, graphics good. A blue circle shows around the
  drift sparks, which the console doesn't draw.
- **Mario Kart World: command-stream desync (Build 73).** With the GPU split, the front thread
  parses command lists as they arrive, while GPU memory maps (`nx2:gpu` 211, sub 101) were
  deferred to the back thread. A list the game mapped and submitted right away could be read
  through the address's previous mapping: in the Build 72 crash, a 12-word entry at
  `0x11a71a01ec` held half-float vertex data (`bd8abd56 bc17bbe3 ...`), its first word was
  parsed as a header with a count in the thousands, and the parser ran on into unbound
  subchannels and garbage state. Every bad entry so far sat at a remapped address (`0x119...`,
  `0x11a...`), never at the game's own (`0x21...`). Maps now run before the lists submitted
  after them are read, as in Lichii build 75 (`MapInOrder`). `EDEN_NX2_MAP_BEFORE_READS=0`
  turns this off for A/B runs. Tested (Fil): still crashed (at 121 s), see Build 74 above.
- **Mario Kart World: sparse texture tiles (`nx2:gpu` 216), from Lichii build 75.** Fil's run
  logged six "pool exhausted" warnings and an unlit, black 3D scene while the sky and karts
  rendered. The game keeps its world textures in 25 sparse regions (about 20 GiB reserved) and
  maps its own memory into their tiles with `ExecuteSparseMapRequests` (216), which Satsuma had
  no handler for; NvRm never even asked, because the GPU characteristics left out
  `NVGPU_GPU_FLAGS_SUPPORT_REMAP`. Instead every GPU write there took a 4 MiB block from the
  graphics pool (about 1.9 GiB under the 12 GiB cap) until it ran dry.
  - The remap flag is reported, and 216 maps a tile to the game's memory (or unmaps it) by
    re-pointing the region's device pages in GPU-thread order. Mapped tiles count as written,
    so the texture cache loads the images over them.
  - Once the game maps tiles into a sparse region, GPU writes outside its tiles are dropped as
    on the console, and the blocks it was given before go back to the pool.
  - 216 into GPU-only regions (no sparse flag) follows Lichii: regions backed on demand keep
    their own memory and still count as written; regions backed whole show the game's memory,
    except where two regions share it.
  - Large single 2D images over sparse tiles get host memory per sparse block where the tiles
    hold data, and only those parts are uploaded. On drivers that can't sample unbound memory
    (AMD) their empty blocks share one zeroed block, and a GPU render into such an image backs
    it whole first (`EDEN_NX2_SPARSE_ALIASED`, `EDEN_NX2_SPARSE_UNBACKED`).
  - `EDEN_NX2_SPARSE_REMAP=0` (listed in `nx2_switches.txt`) goes back to the old behaviour for
    A/B runs: no 216, blocks from the graphics pool as the GPU writes them.
  The log shows `nx2:gpu 216:` lines with the tile totals.
  Tested (Fil, Build 72): the world renders correctly again and the expected 30 FPS is back. By
  52 s: 9,080 calls to 216, 968 MiB of tiles mapped, 19 sparse regions on tiles, no "pool
  exhausted". The game still crashed at 70 s from the separate command-stream desync.
- **Build numbers.** The build number lives in `dist/build_number.txt` and shows in the About
  box and the first log line ("Satsuma Version: Satsuma Build 72"). This is Build 72, after the
  Build 71 release.
- **Ocarina of Time ("Green Hat") fixes and new shader support, from Lichii build 75.**
  - Boot: GPU report timestamps on Switch 2 are global-clock nanoseconds, not Maxwell GPU
    ticks, so the game's CPU/GPU time calibration holds. The kernel's address arbiter gains
    `WaitIfEqual64` (64-bit wait-on-address).
  - Shaders: SPIR-V global atomic max; out-of-range signed SHR fills with the input's sign;
    `CALL.REL.NOINC` callees are captured; register pressure covers more opcodes. Lowered:
    `SHF.R.U64` and `USHF.R.U64` funnel shifts, `RED` with a type on a uniform address,
    `UFLO`/`UBREV`/`UPOPC` on any uniform register, signed `REDUX` min/max, predicated
    uniform compares, `SEL` with a uniform operand (`0xc07`), `0x4a4`, the packed
    float-to-byte conversion (`0x243`) and the `HFMA2` H0_NH1 swap.
  - Native compute storage buffers use their pointer only (the next word is not a size) and
    stop where the mapping does. `EDEN_NX2_REJECT_HASH` rejects a native shader by key or hash.
  Not taken: Lichii's frame-dump tool hooks, and lowering paths Satsuma doesn't have
  (`ISETP.EX` rework, the written-register count, F2I negate). Needs testing.
- **Mario Kart World and Switch Sports regressed with that port** (Fil): frame rate about
  halved, many models black, distant scenery low resolution. Two of its runtime changes are now
  off by default behind switches: GPU report timestamps in nanoseconds
  (`EDEN_NX2_GPU_TIME_NS=1`) and native compute storage buffers bound from their pointer alone,
  up to 8 MiB (`EDEN_NX2_SSBO_POINTER_ONLY=1`). The shader changes stay. Needs testing.
  Tested (Fil): Switch Sports is fine again; Mario Kart World was still broken.
- **Mario Kart World: black models and blocky distance.** Its 63 GPU-only regions (about
  3 GiB) are now backed on demand, and the texture cache took images over blocks nobody had
  written as all zeros. Textures the GPU copies in there never counted as written, so they
  came out black or with only their smallest detail levels. GPU-only regions now count as
  written from the start (only their memory still comes on demand). Needs testing.

## 2026-11-04 (third round)

- **Cyberpunk 2077: register-handle surface atomics** (`0x394`, as `0x593` with the handle
  in a register). With it, all 31 programs Cyberpunk loads before its green screen translate
  (offline, from a shader dump of Fil's run); the run before had two compute dispatches failing.
  Cyberpunk plays its videos with Bink, decoded into Y, Cb and Cr images that a shader
  combines; all-zero images give exactly a flat green. Needs testing.
- **Four-word QMD releases.** A compute dispatch's release with bit 30 clear writes the
  payload and a 64-bit timestamp; Satsuma refused those, and Cyberpunk 2077's queue sync at
  QMD word 53 (`0x00800011`) stopped its compute queue about 85 s in. Now accepted and written
  with the timestamp (max and add releases still one word). Needs testing.
- **Switch 2 Edition DLC laid over the game's files.** Super Mario Bros. Wonder's Switch 2
  Edition DLC (Meetup in Bellabel Park) holds files laid out like the game's own, and the game
  only mounts its RomFS, never the add-on content, so it never saw the DLC. When a Switch 1
  game runs a Switch 2 program, its installed DLC (NAND or registered) now goes over the
  game's files. `EDEN_NX2_EDITION_DLC=0` turns it off. First test (Fil): no DLC. Wonder's
  Switch 2 Edition program keeps the Switch 1 NPDM layout and compression, so nothing in its
  files says Switch 2 and the layering never ran. `EDEN_NX2_EDITION_DLC_<program ID>=1` now
  names such a game. Tested on Wonder (Fil): it crashed about 2 s in. Satsuma runs Wonder's
  Switch 1 program (Switch 1 GPU and controller paths), which can't use the Switch 2 Edition
  files, so the Wonder line was removed. Bellabel Park needs the Switch 2 program, which the
  dump does not contain: its base and update 327680 are both Switch 1 programs (the update
  has the shop prompts for buying the upgrade, `ShowShopForUpgrade`,
  `CheckMeetConditionPurchaseUpgrade`), and the DLC folder holds only the upgrade's data.
- **Per-game display rate.** `EDEN_NX2_VSYNC_HZ_<program ID>` overrides `EDEN_NX2_VSYNC_HZ`
  for one game. The shipped switches cap Super Mario Bros. Wonder at 67 under the 120 Hz
  default (Fil's request).
- **Super Mario Bros. Wonder: fully playable** (Fil).
- **Add-on content answers logged.** Wonder's Switch 2 Edition DLC is installed in NAND but not
  picked up; the log now shows what the add-on content service answers. In two runs the game
  only checked the mount status and never asked for its add-on list.
- **GPU-only regions backed as they're used.** With 12 GiB of console memory, four of Switch
  Sports' six 490 MiB GPU-only regions (`nx2:gpu` 214, where it keeps render targets) got no
  memory, and the water and other rendering broke again. Switch Sports' 9.58 GiB plus those
  2.94 GiB is more than the console's 12 GB, so the console can't be backing them whole.
  They now get memory per block as the GPU writes them, as sparse regions already did.
  `EDEN_NX2_GPU_ONLY_LAZY=0` backs them whole. Tested (Fil): the water is back and the intro
  is about 3x smoother. The log shows no failed regions and 4 MiB of them actually backed.
- **Where byte-offset texture handles come from.** Switch Sports' own NVN driver code builds
  handles as `texture | sampler << 20` from pool indices, never byte offsets, so the
  `index x 32` handles the byte-offset rule catches are likely made by the emulator. One
  suspect: the handle tracker reads `value & mask` as `value` shifted left by the mask's
  trailing zeros. The log now shows each tracked handle that was shifted this way and each
  handle the byte-offset rule changed, to confirm the source in Switch Sports.
- **Splatoon Raiders: fully playable** (Fil).
- **Nintendo Switch Sports Resort: playable** (Fil). Graphics correct and bowling pins back
  since the GPU-only region fix; the Joy-Con 2 prompt still asks again after OK.
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
