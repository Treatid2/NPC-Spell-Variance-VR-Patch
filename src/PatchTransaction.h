#pragma once

#include "PatchPlan.h"

namespace nsv_patch {

enum class PointerLocation { kGetAlpha, kUpdateCombat, kStoredOriginal };

enum class ApplyStatus {
  kCommitted,
  kStateChanged,
  kProtectionPreparationFailed,
  kPublishFailedRolledBack,
  kRecoveryIncomplete,
  kProtectionRestoreFailed,
};

struct ApplyResult {
  ApplyStatus status;
  bool pointersMatchPlan;
  bool pointersMatchObserved;
  bool protectionsRestored;
};

template <class Backend>
[[nodiscard]] ApplyResult ApplyPatch(Backend &a_backend,
                                     const ObservedState &a_observed,
                                     const PatchPlan &a_plan) {
  if (!a_backend.MakeVtableWritable()) {
    return {ApplyStatus::kProtectionPreparationFailed, false, true, true};
  }

  if (!a_backend.MakeStorageWritable()) {
    const bool protectionsRestored = a_backend.RestoreVtableProtection();
    return {protectionsRestored ? ApplyStatus::kProtectionPreparationFailed
                                : ApplyStatus::kProtectionRestoreFailed,
            false, true, protectionsRestored};
  }

  const auto matches = [&](PointerLocation a_location,
                           std::uintptr_t a_expected) {
    return a_backend.Read(a_location) == a_expected;
  };
  const auto matchesObserved = [&] {
    return matches(PointerLocation::kGetAlpha, a_observed.currentGetAlpha) &&
           matches(PointerLocation::kUpdateCombat,
                   a_observed.currentUpdateCombat) &&
           matches(PointerLocation::kStoredOriginal, a_observed.storedOriginal);
  };
  const auto matchesPlan = [&] {
    return matches(PointerLocation::kGetAlpha, a_plan.getAlpha) &&
           matches(PointerLocation::kUpdateCombat, a_plan.updateCombat) &&
           matches(PointerLocation::kStoredOriginal, a_plan.storedOriginal);
  };
  const auto restoreProtections = [&] {
    const bool storage = a_backend.RestoreStorageProtection();
    const bool vtable = a_backend.RestoreVtableProtection();
    return storage && vtable;
  };

  if (!matchesObserved()) {
    const bool protectionsRestored = restoreProtections();
    return {protectionsRestored ? ApplyStatus::kStateChanged
                                : ApplyStatus::kProtectionRestoreFailed,
            false, false, protectionsRestored};
  }

  bool changedGetAlpha = false;
  bool changedStorage = false;
  bool changedUpdateCombat = false;

  // Make the misplaced thunk unreachable before changing its call chain.
  changedGetAlpha = a_backend.CompareExchange(
      PointerLocation::kGetAlpha, a_observed.currentGetAlpha, a_plan.getAlpha);
  if (changedGetAlpha) {
    changedStorage = a_backend.CompareExchange(PointerLocation::kStoredOriginal,
                                               a_observed.storedOriginal,
                                               a_plan.storedOriginal);
  }
  if (changedStorage) {
    changedUpdateCombat = a_backend.CompareExchange(
        PointerLocation::kUpdateCombat, a_observed.currentUpdateCombat,
        a_plan.updateCombat);
  }

  if (changedStorage && changedUpdateCombat && changedGetAlpha &&
      matchesPlan()) {
    const bool protectionsRestored = restoreProtections();
    return {protectionsRestored ? ApplyStatus::kCommitted
                                : ApplyStatus::kProtectionRestoreFailed,
            true, false, protectionsRestored};
  }

  // Undo only values still owned by this attempt; never overwrite a later hook.
  if (changedUpdateCombat) {
    static_cast<void>(a_backend.CompareExchange(
        PointerLocation::kUpdateCombat, a_plan.updateCombat,
        a_observed.currentUpdateCombat));
  }
  if (changedStorage) {
    static_cast<void>(a_backend.CompareExchange(
        PointerLocation::kStoredOriginal, a_plan.storedOriginal,
        a_observed.storedOriginal));
  }
  if (changedGetAlpha) {
    static_cast<void>(a_backend.CompareExchange(PointerLocation::kGetAlpha,
                                                a_plan.getAlpha,
                                                a_observed.currentGetAlpha));
  }

  const bool pointersRestored = matchesObserved();
  const bool protectionsRestored = restoreProtections();
  if (!pointersRestored) {
    return {ApplyStatus::kRecoveryIncomplete, false, false,
            protectionsRestored};
  }
  if (!protectionsRestored) {
    return {ApplyStatus::kProtectionRestoreFailed, false, true, false};
  }
  return {ApplyStatus::kPublishFailedRolledBack, false, true, true};
}

} // namespace nsv_patch
