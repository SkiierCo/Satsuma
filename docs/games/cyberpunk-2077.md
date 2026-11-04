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

## Next

Test the 2026-11-04 build for audio. The game uses DLSS too, so tensor emulation may also
matter once it renders.
