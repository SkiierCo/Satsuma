# Cyberpunk 2077

Program ID: `0400DDB020648000`. Status: not recorded (see [compatibility](../compatibility.md)).

## Symptoms

- No audio.

## Findings

Switch 2 games decode Opus through the hardware audio codec: `hwopus` command 100 opens an
`nn::codec::hwacod::IClientSession`, which works in batches (a decoder manager with I/O
buffers and an event; packets queued, decoded together, and read back as PCM).

Satsuma answered command 100 with a stub that only logged. The 2026-11-04 log shows the
game sending session commands 1, 0 (decoder setup: 48 kHz, 1-2 channels, 20 ms frames)
and 10 (open the manager session), then never using the manager session: it rejected the
stub's replies. Earlier, the stub's zero replies also opened decoders at 0 Hz; those are
now defaulted to 48 kHz and checked.

Lichii build 70 implements the protocol: shared-memory task and completion queues driving
a batch Opus decoder. It is ported, with Lichii's tests.

Since the codec port the game launches, to a green screen. The 2026-11-04 log shows five
compute programs rejected at about 30 and 40 seconds: texel fetches through a
uniform-register handle (`0xf66`, three) and a register handle (`0x367`), and a 1D-buffer
surface store through a register handle (`0x99c`). All five now lower. The same log also
has 96 Fermi 2D "source depth is not one" asserts and 85 storage-buffer tracking warnings;
not yet looked at. No video decoder activity.

## 2026-11-04 runs (later builds)

- Bink: the game plays its videos with Bink (`.bk2`), decoded into Y, Cb and Cr images that a
  shader combines. All-zero images give exactly a flat green.
- Shader dumps of Fil's runs: 31, then 62 programs loaded; with the `0x394` surface atomic
  lowering all of them translate offline.
- A compute queue sync used a four-word QMD release (word 53 = `0x00800011`), which Satsuma
  refused about 85 s in. Four-word releases are now accepted.
- Latest run (after that fix): the game gets further. At about 43 s one compute program
  translates but its SPIR-V uses an instruction the backend doesn't implement, and seven
  graphics pipelines fail to track a bindless texture handle that the shader loads from global
  memory (`LoadGlobal32`). 18 compute dispatches are skipped as a result.

## Next

Implement the missing SPIR-V instruction and follow bindless handles loaded from global memory.
