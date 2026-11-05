# Mario Kart World

Program ID: `0400C3F00006E000`. Status: in game + (see [compatibility](../compatibility.md)).

## Symptoms

- The 3D scene is unlit and black while the sky and the karts render (Fil, 2026-11-05). The log
  has six "nx2:gpu sparse region at ...: pool exhausted, GPU writes to its unbacked blocks are
  dropped" warnings.
- The command stream desyncs and the game crashes (unbound subchannel, bad syncpoint IDs, then
  garbage GPU state). Separate from the above; not investigated here.

## Findings

**Memory layout.** The game creates 25 sparse regions (`nx2:gpu` 214 with the sparse flag,
about 20 GiB reserved) and 63 GPU-only regions (no sparse flag, about 3 GiB). Both are backed
on demand: a 4 MiB block from the graphics pool (`Pool::SystemNonSecure`, about 1.9 GiB under
the 12 GiB cap) when the GPU first writes there.

**GPU-only regions.** Images the GPU copied into them never counted as written, so the texture
cache took them as all zeros: black models and only the smallest detail levels. Fixed on
2026-11-05: GPU-only regions count as written from the start.

**Sparse regions.** On the console the game puts its own memory into their tiles with
`ExecuteSparseMapRequests` (216, behind `nvnMemoryPoolMapVirtual`); nothing else gets memory
there, and GPU writes outside its tiles are dropped. Satsuma had no 216 handler, and NvRm never
called it: without `NVGPU_GPU_FLAGS_SUPPORT_REMAP` in the GPU characteristics it fails every
remap before asking the server. The game's world textures (world-sized R8/RG16/R16 masks it
fills in its own memory before mapping them) were never mapped, so they read zero, while every
GPU write into the regions took graphics-pool blocks until the pool ran out.

Ported from Lichii build 75 on 2026-11-05: the remap flag, 216 (tiles re-point the region's
device pages at the game's memory), dropped writes outside the tiles with the earlier blocks
given back, and per-sparse-block host memory for the large images over the tiles.
`EDEN_NX2_SPARSE_REMAP=0` restores the old behaviour for comparison.

## Build 72 test (Fil, 2026-11-05)

The game looks right again and runs at the expected 30 FPS. The log shows 216 working: by 52 s
there were 9,080 calls (8,982 maps, 98 unmaps), 968 MiB of the game's tiles mapped, 19 sparse
regions switched to tiles, and no "pool exhausted" warnings. The 7,917 maps into GPU-only
regions were left out, as those regions keep their own memory. The game still crashed at 70 s:
an unbound subchannel at 69.4 s, then invalid Fermi2D registers, syncpoint ID 0x500 and an
image of garbage size. That is the command-stream desync.

**Command-stream desync.** The parser itself doesn't slip: entries hold the wrong memory. In
the Build 72 crash the implausible header came from entry 1023 at `0x11a71a01ec` (12 words),
whose memory held half-float vertex data, and the 2026-11-04 crash7 entry at `0x11a00e62c4`
held random data too. Both are at addresses the game maps its memory to with `nx2:gpu` 211
(not at its own addresses, `0x21...`). With the GPU split those maps ran on the back thread,
after the front had already parsed lists submitted behind them, through the old mapping. Build
73 makes the front wait for each map before it reads later lists (`MapInOrder`, from Lichii
build 75; `EDEN_NX2_MAP_BEFORE_READS=0` turns it off).

**Build 73 test (Fil).** The terrain started glitching, then the game crashed at 121 s with the
desync. This time the bad entry was the reported one, so its mapping history was in the log:
memory at `0x11be802000` (0x556000 bytes) was mapped, unmapped, replaced by a 0x1546000-byte
memory, unmapped again, and 0.58 s before the crash a 2-page memory at `0x11bea8b000` was
mapped through the first memory's device range. Destroyed memory never released its device
ranges, and a device range keeps the pages its memory had when it was mapped, so the new
memory showed old pages (command words read as vertex data). The same stale pages would give
geometry and textures old data, which fits the terrain glitches. Build 74 releases a destroyed
memory's device ranges once no GPU mapping uses it, and reuses a device range only while it
still reaches the CPU address's current memory (a warning is logged when it doesn't).

**Build 74 test (Fil).** No crash in over 21 minutes, no desync report and no stale-range
warning: releasing destroyed memory's ranges was enough. Graphics good, 30 FPS.

## Next

- A blue circle around the drift sparks (the console shows only the sparks).
- Frame rate above 30 FPS.
