# Changelog

## 1.0.2 - 2026-09-09

- Stop dependent rollback when a newer hook may still reach the corrected
  thunk.
- Preserve the thunk's compatible original-function chain during contested
  recovery.
- Report pointer drift during writable-page preparation without claiming the
  earlier tuple was verified.
- Retain simultaneous pointer and page-protection recovery failures in the
  terminal diagnostic.
- Exercise ordered publication, actual competing values, protection restore
  attempts, and compound failures in the deterministic suite.

## 1.0.1 - 2026-09-09

- Replace a compare/exchange-based pointer observation with a true atomic load.
- Avoid an access violation while validating Skyrim's read-only vtable pages.
- Exercise the pointer observation against a `PAGE_READONLY` allocation.

## 1.0.0 - 2026-09-08

- Restore Skyrim VR `Character::GetAlpha` at vtable slot `0xE4`.
- Move the NPC Spell Variance 2.7.0 `UpdateCombat` hook to VR slot `0xE6`.
- Preserve hook chaining with any function already installed at `0xE6`.
- Fail closed on unsupported DLLs, altered hook state or signature mismatch.
- Require the exact supported DLL hash and safely bounded mapped-memory reads.
- Reject null, recursive, aliased or non-executable `UpdateCombat` targets.
- Bind publication to the validated state and verify rollback and page
  protection outcomes.
- Remove the misplaced E4 hook before changing its call chain, then publish
  the corrected E6 hook last.
