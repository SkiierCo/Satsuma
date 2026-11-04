# Persistent save states

Status: implementation in progress. There is no Save State/Load State action or hotkey.
The archive and component codecs below are not a complete emulator checkpoint.

## Implemented

- `core/savestate/archive`: explicit little-endian fields; title ID, program/update hash and
  configuration hash; versioned component records; Zstandard chunks up to 16 MiB; SHA-256 checks
  covering metadata and compressed payloads; ordered chunk indices and a completion checksum.
  Container validation checks every chunk before exposing a reader. It rejects truncation,
  trailing data, unknown versions, oversized records and identity mismatches. Reader memory is
  bounded by chunk size, without buffering the whole guest RAM image.
- `core/savestate/cpu`: process/thread identity, ARM architecture, GPRs, SP, PC, PSTATE,
  vector registers, FPCR/FPSR and both guest TLS registers. Fields are encoded individually;
  host pointers, C++ object representations and ABI padding are excluded. This is a register
  codec; it does not yet stop or reconstruct live CPU threads.
- `NativeQmdScheduler::SaveState/LoadState`: the cross-channel native compute dependency table,
  including queued decrements, descriptor words and completion flags. Decode validates into a
  temporary table before replacing the existing one. The table resets when a new GPU session
  starts, so one game cannot inherit another game's dependency counts.
- `core/savestate/kernel_handles`: handle generation counters, allocation/high-water counts,
  ordered free lists, reserved IPC slots and stable kernel object IDs. Capture reads the actual
  handle table at a quiescent boundary. Restore resolves all references first and acquires them
  into a fresh candidate table, rolling back if an object is inactive. Kernel object factories,
  process/thread/wait reconstruction and a complete kernel registry are still required.
- `core/savestate/service_sessions`: session/domain routing, closed virtual handle slots,
  handler aliases, pending domain conversion and service-manager initialization state. Handler
  resolution preserves shared ownership and rejects missing references before changing a fresh
  candidate session manager. Concrete handler state/factories and in-flight IPC continuations
  are not implemented by this routing codec.

The format checks component presence, not whether an arbitrary payload describes a valid
machine. Component-specific decode and cross-reference validation must finish before restore.
The current archive functions use caller-owned streams; they do not publish slot files.

## Required before exposing Save/Load

1. A machine-wide checkpoint boundary: stop guest CPUs and service execution, quiesce timing,
   drain both GPU command threads, wait for host GPU work and download authoritative GPU memory.
   Ordinary GUI Pause alone is insufficient. Resume every participant if capture fails.
2. Sparse RAM capture and reconstruction, preserving aliases, permissions, backing ownership,
   guest allocations and mappings without committing untouched pages or duplicating 12 GiB.
3. Kernel object reconstruction with stable IDs: processes, threads, handle serials/free lists,
   scheduler queues, waits, synchronization, arbitration, sessions, shared/transfer memory,
   resource limits, TLS and outstanding SVC/IPC continuations. Host fibers cannot be copied
   across process restarts; continuations must be represented explicitly.
4. Service session factories and versioned state for every live interface, including pending
   replies and references to kernel objects, filesystem cursors, graphics image/memory objects,
   display events and syncpoint callbacks. Unsupported live services must reject capture.
5. The rest of GPU state: channels, address spaces, DMA cursors/acquires, engine registers,
   macros/data RAM, command uploads, syncpoints/fences and presentation queues. Rebuild host
   Vulkan resources and invalidate/recreate caches; do not persist Vulkan handles or host JITs.
6. Audio decoder/mixer and timing queues/clocks, with stable callback identities and deadlines
   restored relative to the checkpoint rather than the host's current wall-clock epoch.
7. Full semantic preflight and transactional restoration into a new machine, durable temporary
   file writes followed by atomic slot replacement, cancellation/error recovery, then UI and
   shortcuts. Round-trip live games, restart the process and repeat against a compatible build.

Cross-build compatibility is governed by per-component schemas and explicit migrations, not
the executable name. Different game updates or incompatible configurations must be rejected.
No future build is presumed compatible merely because the archive format number matches.
State files will contain guest memory and must remain user data, outside release packages.

## Verification

`tests.exe "[savestate],[qmd]"` exercises register bits, truncation at every byte, single-byte
archive corruption at every position, identity mismatches, bounded chunk sizes, multiple RAM
chunks, changed backing streams, cross-channel dependencies and failed QMD restore rollback.
Kernel/service fixtures cover free-list cycles, handle reservations and generation counters,
aliased object references, missing/inactive objects, domain holes and reference rollback.
These tests do not establish that a complete game can be saved or restored.
