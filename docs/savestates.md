# Persistent save states

Status: implementation in progress. There is no Save State/Load State action or hotkey yet.
The planned UI (Fil, 2026-11-05): Emulation menu Save State / Load State with slots 1-4, F7 saves
and F8 loads the selected slot (both rebindable), files in `user/savestates/<title id>/`. Scope:
all games. A state loads in a later build when that build's component formats are unchanged;
otherwise it is rejected with the reason.

## How a state is taken

A checkpoint stops every user thread at a point a file can describe, while HLE services, GPU and
timing keep running until the threads have stopped (`core/savestate/checkpoint`):

- `Svc::Call` does not start a new SVC during a checkpoint: the thread's PC goes back to its svc
  instruction and the thread stops (`SuspendType::Checkpoint`), so the SVC runs afterwards.
- An SVC that finishes during a checkpoint writes its results first, then the thread stops.
- Guest code stops at an instruction boundary (`PhysicalCore::RunThread`, cores are interrupted).
- `SleepThread`, `WaitSynchronization` and `ArbitrateLock` waits are cancelled; the SVC discards
  its results and runs again from the svc instruction. These three re-check their condition, so
  running them again is safe.
- `WaitProcessWideKeyAtomic` already released its mutex, and `WaitForAddress` may have
  decremented its value, so running them again is not safe. They stay blocked and are recorded:
  key, address, value, mutex owner when already signalled, remaining timeout.
- IPC requests to HLE services complete before the client stops.

The gates check the flag under the scheduler lock, so stopping and resuming cannot race. A thread
that does not reach a stop point within the timeout is reported with the SVC it is in, and every
thread resumes. Multicore CPU emulation is required.

## Implemented

- `archive`: container with explicit little-endian fields; title ID, program/update hash and
  configuration hash; Zstandard chunks up to 16 MiB; SHA-256 over metadata and payloads.
- `stream`: typed component payloads; every section has a tag and a version that a reader
  checks, so a payload from an incompatible schema is rejected instead of misread.
- `checkpoint`: the stop protocol above.
- `memory`: the process address space. Capture records the region layout, the memory block list,
  virtual-to-physical runs and page contents; a physical page mapped at two addresses (stack
  aliases, module code) is stored once and all-zero pages are omitted. Restore replays the
  layout into the freshly loaded process with the kernel's own operations (heap size, physical
  memory, main and alias stacks, module code mappings, thread-local pages at their saved
  addresses, permissions, attributes), checks that the block list equals the saved one, then
  copies contents through the new mapping. Physical addresses are only identities; fresh pages
  are used. The code is loaded at its saved address (`KProcess::s_restore_code_address`).
- `kernel_state`: threads (ID, registers, priority, affinity, TLS slot, hold, pause by the
  game), the game's events and transfer memory, readable event states and the handle table.
  Restored threads keep their thread IDs. A thread restored inside a condition-variable or
  address wait is put back into the kernel's tree (or onto its mutex owner) with a wait queue
  that writes the SVC result to X0 when it wakes. Objects owned by services are left to the
  services component; any other object makes capture fail with its type.
- `kernel_handles`, `service_sessions`: handle table and session/domain routing codecs.
- `NativeQmdScheduler::SaveState/LoadState`: native compute dependency table.

## Not implemented yet

1. Services: state and factories for every live HLE interface and the objects they give the
   game (events, shared memory, sessions), including `nx2:*`. Unsupported live services must
   make capture fail with the service name.
2. GPU: drain both GPU threads, wait for the host GPU, write GPU-modified memory back to guest
   memory; channels, address spaces, engine registers, macros, syncpoints, presentation queues.
   Host caches are rebuilt from guest memory.
3. Audio renderer and timing (CoreTiming events relative to the checkpoint).
4. Restore-mode boot: load the same title, skip `KProcess::Run`, restore memory, kernel objects,
   services, GPU and audio, then start the threads.
5. Slot files (temporary file then atomic replace), menu items, hotkeys, error messages.

Cross-build compatibility is governed by the per-component section versions. Different game
updates or incompatible configurations are rejected. State files contain guest memory and are
user data, never part of release packages.

## Verification

`tests.exe "[savestate],[qmd]"` covers the archive (truncation at every byte, corruption at
every position, identity mismatches, chunk limits), the stream (field round trips, wrong tags and
versions, trailing data), the address-space and kernel-object formats (round trips, truncation,
invalid runs and thread states), handle tables, service session routing and the QMD table.
No game has been saved or restored yet.
