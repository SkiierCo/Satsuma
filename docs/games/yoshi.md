# Yoshi

Program ID: `0400245021D2C000`. Status: in game (see [compatibility](../compatibility.md)).

## Symptoms

- Main menu works. Gameplay and cutscenes render black, with flashing red geometry.
- On 2026-11-04 the game crashed just after a cutscene. It read through a null pointer
  (address `0x98`, PC `0x81a560b0`) and aborted with 2162-0001.
- With the 2026-11-04 shader build the main menu background turned black once. Not yet
  confirmed after the later fixes.

## Findings

**Shader code reads as zeros.** Many gameplay programs read zero instructions up to a page
boundary, or branch outside what was captured, so they are rejected and their draws and
dispatches skipped. The game copies its shader code into place with the GPU's DMA engine.

- The cache probe (2026-11-04) showed the zeroed pages are held by the buffer cache, with
  no texture images and no sparse memory involved.
- The DMA copy and clear paths translated only the first byte of each range and treated
  the rest as one unbroken run of device memory. Over scattered pages they read and wrote
  the wrong ones. Fixed: such ranges go page by page.
- After that fix, the zeroed pages showed as GPU-written in the buffer cache: the code was
  only there, and the shader reader read guest memory. Fixed: GPU-written pages are copied
  back from the buffer cache first (`EDEN_NX2_SHADER_FLUSH=0` turns it off).
- An earlier attempt flushed every cache before reading and wrote zeros over good menu
  code; that is consistent with the DMA bug above, now fixed.

**The crash** most likely follows from the skipped compute shaders. The game read back
over 52,000 GPU-written pages just before it, and several compute dispatches had been
skipped seconds earlier.

**Instructions** lowered for Yoshi: TLD4 gathers with uniform handles on 2D arrays and with
offsets (`0xf63`), ATOMG exchange (`0x3a8`), SURED adds into 1D buffer images (`0x59f`),
I2FP with an immediate (`0x845`, a main-menu compute shader). On the 2026-11-03 dump,
1508 of 1594 programs lower; most of the rest are the zeroed programs above.

## Next

Test the 2026-11-04 build: main menu background, a cutscene, gameplay. Send the log; its
"Native program" lines show whether any shader still reads as zeros.
