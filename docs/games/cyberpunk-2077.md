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

## 2026-11-05 (Build 79 run, fixes in Build 81)

- The run still shows the green screen. Two compute pipelines failed in the SPIR-V backend, which
  only said "SPIR-V Instruction"; an offline probe on the shader dump found 16-bit global loads
  (`LoadGlobalU16`) through pointers read from memory. 8- and 16-bit global loads are now
  emitted.
- Five vertex shaders start with `ULDC.64 UR4, c[0x19][0x0]` or `c[0x1f][0x0]` and then load
  vertex data through that pointer. Banks 24-31 are NVN2's root tables (written with 3D methods
  0x504/0x508); tables 1-7 are now readable, from bank 17 at table * 0x100. Whether the game
  writes tables 1 and 7 shows in the log as "Root table N (bank M) first written".
- Three compute programs were rejected at `REDUX.AND`, `LDG.E.STRONG.GPU` with a uniform base,
  and `SURED` with a register handle (0x3a0). All three now lower.
- Offline, 126 of 130 dumped compute programs now reach SPIR-V. Left: a global atomic add with
  no routable storage buffer (two programs), a global store register form and a shared atomic
  form.

## 2026-11-05 Build 82 run (Fil)

Still the green screen, now at 0 FPS. The log of that session filled up on Yoshi before
Cyberpunk ran, so whether root tables 1 and 7 are written is still unknown.

## Next

Fil's Build 81 run: check whether the green screen changes, whether root tables 1 and 7 are
written, and which compute dispatches are still skipped. Then the remaining compute forms and
the bindless texture handles loaded from global memory (`LoadGlobal32`, six graphics pipelines).
