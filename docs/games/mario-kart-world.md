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

## Next

- Run the game on the new build: the log should show `nx2:gpu 216:` lines (calls, maps,
  unmaps, MiB of tiles mapped, MiB of blocks released) and no "pool exhausted" warnings.
