# NPC Spell Variance VR Patch

This is a temporary compatibility patch for **NPC Spell Variance - Spell
Variety AI 2.7.0** on **Skyrim VR 1.4.15**.

NPC Spell Variance 2.7.0 installs its `UpdateCombat` hook in Character vtable
slot `0xE4`. That slot is `UpdateCombat` on Skyrim Special Edition, but it is
`GetAlpha` on Skyrim VR; VR uses slot `0xE6` for `UpdateCombat`.

The misplaced `void UpdateCombat(Actor*)` hook replaces the floating-point
`GetAlpha` function. Callers consequently receive an undefined value in
`XMM0`. PLANCK's collision phase-through feature exposes the fault when it
reads that value and derives a temporary actor alpha, producing glowing eyes
or invisible actors.

This plugin restores the preserved Skyrim VR `GetAlpha` function to slot
`0xE4`, moves the NPC Spell Variance hook to `0xE6`, and updates that hook's
original-function target so it chains the real VR `UpdateCombat` implementation.

## Compatibility

- Skyrim VR 1.4.15 only.
- NPC Spell Variance - Spell Variety AI 2.7.0 only.
- SKSEVR 2.0.12 or later.
- Address Library for SKSE Plugins is required by CommonLibSSE-NG.

The patch is intentionally tied to the released 2.7.0 DLL identity, code
signature, hook pointer and preserved original function. Its exact supported
DLL SHA-256 is
`F1CB34F26F49FAFCB981CA55FA83E9119E606BBCE93D7A135C60DCDEEAD47CD6`.
If any precondition does not match, it logs the mismatch and changes nothing.

The existing VR `UpdateCombat` target must also be a distinct executable
address. Pointer publication uses compare/exchange operations bound to the
state that was validated. A competing hook causes a no-op or a conditional
rollback; the patch never deliberately overwrites a newer value. Every page
protection transition and residual pointer state is checked before the log
describes the outcome as committed or restored.

The misplaced E4 hook is removed before its stored original is changed. The
corrected E6 hook is published last, so the thunk is never intentionally live
with an original function from the wrong virtual slot.

This package does not redistribute or modify NPC Spell Variance files. Install
it as a separate mod after NPC Spell Variance. Remove this patch after NPC
Spell Variance publishes and you install a release with a runtime-aware VR
`UpdateCombat` hook.

## Why this is separate from the PLANCK patch

PLANCK did not create the bad `GetAlpha` value. It consumed the value returned
by another plugin's misplaced hook. Defensive validation in PLANCK can hide
the symptom, but it cannot repair the producer and may obscure similar faults.
The actual compatibility correction therefore belongs next to NPC Spell
Variance.

## Evidence

The released NPC Spell Variance 2.7.0 DLL and its matching PDB show:

- PE timestamp `0x6A6A6035`, image size `0xC6000`;
- complete DLL SHA-256
  `F1CB34F26F49FAFCB981CA55FA83E9119E606BBCE93D7A135C60DCDEEAD47CD6`;
- `ActorHook::UpdateCombat(RE::Actor*)` at RVA `0x34840`;
- installation of that thunk at vtable byte offset `0x720` (`0xE4 * 8`);
- original-function storage at RVA `0xBD540`.

The upstream report is on the
[NPC Spell Variance Nexus bug tracker](https://www.nexusmods.com/skyrimspecialedition/mods/132097?tab=bugs).

## Building

Requirements:

- Visual Studio 2022 or newer with the C++ workload;
- CMake 3.25 or newer;
- vcpkg;
- CommonLibSSE-NG 7.4.0 (fetched automatically), or an existing checkout of
  that release.

Configure with a local CommonLibSSE-NG checkout:

```powershell
cmake -S . -B build -A x64 `
  -DCOMMONLIBSSE_NG_SOURCE_DIR=C:\path\to\CommonLibSSE-NG
cmake --build build --config Release
ctest --test-dir build -C Release --output-on-failure
```

## Packaging

Nexus packages include the complete tracked source tree as ordinary files
under `Source\`. Do not embed a source ZIP, 7z or other archive inside the
distributable archive; Nexus scanning may reject nested archives.

## License

NPC Spell Variance VR Patch is licensed under GPL-3.0-or-later. NPC Spell
Variance, Skyrim, SKSE, CommonLibSSE-NG and PLANCK remain the property of their
respective authors and are not included.
