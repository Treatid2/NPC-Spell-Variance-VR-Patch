#pragma once

#include "PatchPlan.h"

namespace nsv_patch {

enum class PointerLocation { kGetAlpha, kUpdateCombat, kStoredOriginal };

enum class ApplyStatus {
  kCommitted,
  kStateChanged,
  kProtectionPreparationFailed,
  kPublishIncomplete,
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

  if (!a_backend.MakeVtableWritable()) {
    return {ApplyStatus::kProtectionPreparationFailed, false,
            matchesObserved(), true};
  }

  if (!a_backend.MakeStorageWritable()) {
    const bool protectionsRestored = a_backend.RestoreVtableProtection();
    return {protectionsRestored ? ApplyStatus::kProtectionPreparationFailed
                                : ApplyStatus::kProtectionRestoreFailed,
            false, matchesObserved(), protectionsRestored};
  }

  const auto restoreProtections = [&] {
    const bool storage = a_backend.RestoreStorageProtection();
    const bool vtable = a_backend.RestoreVtableProtection();
    return storage && vtable;
  };
  const auto finish = [&](ApplyStatus a_status) {
    const bool protectionsRestored = restoreProtections();
    const bool pointersMatchPlan = matchesPlan();
    const bool pointersMatchObserved = matchesObserved();
    auto finalStatus = a_status;
    if (!protectionsRestored) {
      finalStatus = ApplyStatus::kProtectionRestoreFailed;
    } else if (a_status == ApplyStatus::kCommitted && !pointersMatchPlan) {
      finalStatus = ApplyStatus::kPublishIncomplete;
    }
    return ApplyResult{
        finalStatus,
        pointersMatchPlan, pointersMatchObserved, protectionsRestored};
  };

  if (!matchesObserved()) {
    return finish(ApplyStatus::kStateChanged);
  }

  // Make the misplaced thunk unreachable before changing its call chain.
  if (!a_backend.CompareExchange(PointerLocation::kGetAlpha,
                                 a_observed.currentGetAlpha,
                                 a_plan.getAlpha)) {
    return finish(ApplyStatus::kPublishIncomplete);
  }

  // Installation runs in the serialized SKSE post-load dispatcher. These
  // checks fail closed if a non-cooperating writer is nevertheless observed.
  if (!matches(PointerLocation::kGetAlpha, a_plan.getAlpha) ||
      !matches(PointerLocation::kUpdateCombat,
               a_observed.currentUpdateCombat) ||
      !a_backend.CompareExchange(PointerLocation::kStoredOriginal,
                                 a_observed.storedOriginal,
                                 a_plan.storedOriginal)) {
    return finish(ApplyStatus::kPublishIncomplete);
  }

  if (!matches(PointerLocation::kGetAlpha, a_plan.getAlpha) ||
      !matches(PointerLocation::kStoredOriginal, a_plan.storedOriginal) ||
      !a_backend.CompareExchange(PointerLocation::kUpdateCombat,
                                 a_observed.currentUpdateCombat,
                                 a_plan.updateCombat)) {
    return finish(ApplyStatus::kPublishIncomplete);
  }

  return finish(matchesPlan() ? ApplyStatus::kCommitted
                              : ApplyStatus::kPublishIncomplete);
}

} // namespace nsv_patch
