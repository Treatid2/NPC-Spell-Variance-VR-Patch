# Changelog

## 1.0.0 - 2026-09-08

- Restore Skyrim VR `Character::GetAlpha` at vtable slot `0xE4`.
- Move the NPC Spell Variance 2.7.0 `UpdateCombat` hook to VR slot `0xE6`.
- Preserve hook chaining with any function already installed at `0xE6`.
- Fail closed on unsupported DLLs, altered hook state or signature mismatch.
