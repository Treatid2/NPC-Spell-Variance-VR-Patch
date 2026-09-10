#include "BinaryIdentity.h"
#include "PatchPlan.h"
#include "PatchTransaction.h"

#include <array>
#include <iostream>
#include <vector>

namespace {
constexpr std::uintptr_t kNsvBase = 0x180000000;
constexpr std::uintptr_t kSkyrimBase = 0x140000000;

constexpr nsv_patch::ObservedState ValidState() {
  return {
      nsv_patch::kExpectedTimestamp,
      nsv_patch::kExpectedImageSize,
      kNsvBase,
      kSkyrimBase,
      kNsvBase + nsv_patch::kUpdateCombatThunkRva,
      kSkyrimBase + 0x62E570,
      kSkyrimBase + nsv_patch::kSkyrimGetAlphaRva,
      true,
      true,
  };
}

constexpr std::size_t Index(nsv_patch::PointerLocation a_location) {
  return static_cast<std::size_t>(a_location);
}

struct FakeBackend {
  struct ExchangeMutation {
    bool enabled = false;
    nsv_patch::PointerLocation trigger =
        nsv_patch::PointerLocation::kGetAlpha;
    int triggerCall = 0;
    nsv_patch::PointerLocation target =
        nsv_patch::PointerLocation::kGetAlpha;
    std::uintptr_t value = 0;
  };

  std::array<std::uintptr_t, 3> values{};
  std::array<int, 3> compareExchangeCalls{};
  std::vector<nsv_patch::PointerLocation> exchangeOrder{};
  ExchangeMutation beforeExchangeMutation{};
  ExchangeMutation exchangeMutation{};
  bool makeVtableWritable = true;
  bool makeStorageWritable = true;
  bool restoreVtable = true;
  bool restoreStorage = true;
  bool mutateDuringVtablePreparation = false;
  bool mutateDuringStoragePreparation = false;
  bool mutateDuringVtableRestore = false;
  bool mutateDuringStorageRestore = false;
  nsv_patch::PointerLocation preparationMutationTarget =
      nsv_patch::PointerLocation::kUpdateCombat;
  std::uintptr_t preparationMutationValue = 0;
  int restoreVtableCalls = 0;
  int restoreStorageCalls = 0;

  explicit FakeBackend(const nsv_patch::ObservedState &a_state)
      : values{a_state.currentGetAlpha, a_state.currentUpdateCombat,
               a_state.storedOriginal} {}

  bool MakeVtableWritable() {
    if (mutateDuringVtablePreparation) {
      values[Index(preparationMutationTarget)] = preparationMutationValue;
    }
    return makeVtableWritable;
  }
  bool MakeStorageWritable() {
    if (mutateDuringStoragePreparation) {
      values[Index(preparationMutationTarget)] = preparationMutationValue;
    }
    return makeStorageWritable;
  }
  bool RestoreVtableProtection() {
    ++restoreVtableCalls;
    if (mutateDuringVtableRestore) {
      values[Index(preparationMutationTarget)] = preparationMutationValue;
    }
    return restoreVtable;
  }
  bool RestoreStorageProtection() {
    ++restoreStorageCalls;
    if (mutateDuringStorageRestore) {
      values[Index(preparationMutationTarget)] = preparationMutationValue;
    }
    return restoreStorage;
  }

  std::uintptr_t Read(nsv_patch::PointerLocation a_location) const {
    return values[Index(a_location)];
  }

  bool CompareExchange(nsv_patch::PointerLocation a_location,
                       std::uintptr_t a_expected, std::uintptr_t a_value) {
    const auto index = Index(a_location);
    ++compareExchangeCalls[index];
    exchangeOrder.push_back(a_location);
    if (beforeExchangeMutation.enabled &&
        beforeExchangeMutation.trigger == a_location &&
        beforeExchangeMutation.triggerCall == compareExchangeCalls[index]) {
      values[Index(beforeExchangeMutation.target)] =
          beforeExchangeMutation.value;
    }
    const bool succeeded = values[index] == a_expected;
    if (succeeded) {
      values[index] = a_value;
    }
    if (exchangeMutation.enabled && exchangeMutation.trigger == a_location &&
        exchangeMutation.triggerCall == compareExchangeCalls[index]) {
      values[Index(exchangeMutation.target)] = exchangeMutation.value;
    }
    return succeeded;
  }
};
} // namespace

[[nodiscard]] bool Check(bool a_condition, const char *a_description) {
  if (!a_condition) {
    std::cerr << "FAILED: " << a_description << '\n';
  }
  return a_condition;
}

int main() {
  bool passed = true;
  const auto valid = ValidState();
  const auto ready = nsv_patch::MakePatchPlan(valid);
  passed &= Check(ready.status == nsv_patch::PlanStatus::kReady,
                  "valid state is ready");
  passed &= Check(ready.getAlpha == kSkyrimBase + nsv_patch::kSkyrimGetAlphaRva,
                  "GetAlpha restoration target");
  passed &=
      Check(ready.updateCombat == kNsvBase + nsv_patch::kUpdateCombatThunkRva,
            "UpdateCombat hook target");
  passed &= Check(ready.storedOriginal == valid.currentUpdateCombat,
                  "existing UpdateCombat chain is preserved");

  auto unsupported = valid;
  unsupported.timestamp = 0;
  passed &= Check(nsv_patch::MakePatchPlan(unsupported).status ==
                      nsv_patch::PlanStatus::kUnsupportedBuild,
                  "unsupported build is rejected");

  auto displaced = valid;
  displaced.currentGetAlpha = kSkyrimBase + nsv_patch::kSkyrimGetAlphaRva;
  passed &= Check(nsv_patch::MakePatchPlan(displaced).status ==
                      nsv_patch::PlanStatus::kUnexpectedGetAlphaHook,
                  "unexpected GetAlpha hook is rejected");

  auto badOriginal = valid;
  badOriginal.storedOriginal = kSkyrimBase + 0x1234;
  passed &= Check(nsv_patch::MakePatchPlan(badOriginal).status ==
                      nsv_patch::PlanStatus::kUnexpectedOriginalGetAlpha,
                  "unexpected preserved GetAlpha is rejected");

  auto applied = valid;
  applied.currentGetAlpha = kSkyrimBase + nsv_patch::kSkyrimGetAlphaRva;
  applied.currentUpdateCombat = kNsvBase + nsv_patch::kUpdateCombatThunkRva;
  applied.storedOriginal = kSkyrimBase + 0x62E570;
  applied.storedOriginalExecutable = true;
  passed &= Check(nsv_patch::MakePatchPlan(applied).status ==
                      nsv_patch::PlanStatus::kAlreadyApplied,
                  "applied state is idempotent");

  auto nullChain = valid;
  nullChain.currentUpdateCombat = 0;
  nullChain.currentUpdateCombatExecutable = false;
  passed &= Check(nsv_patch::MakePatchPlan(nullChain).status ==
                      nsv_patch::PlanStatus::kUnsafeUpdateCombatChain,
                  "null UpdateCombat chain is rejected");

  auto recursiveChain = valid;
  recursiveChain.currentUpdateCombat =
      kNsvBase + nsv_patch::kUpdateCombatThunkRva;
  passed &= Check(nsv_patch::MakePatchPlan(recursiveChain).status ==
                      nsv_patch::PlanStatus::kUnsafeUpdateCombatChain,
                  "recursive UpdateCombat chain is rejected");

  auto aliasedChain = valid;
  aliasedChain.currentUpdateCombat =
      kSkyrimBase + nsv_patch::kSkyrimGetAlphaRva;
  passed &= Check(nsv_patch::MakePatchPlan(aliasedChain).status ==
                      nsv_patch::PlanStatus::kUnsafeUpdateCombatChain,
                  "GetAlpha chain alias is rejected");

  auto nonExecutableChain = valid;
  nonExecutableChain.currentUpdateCombatExecutable = false;
  passed &= Check(nsv_patch::MakePatchPlan(nonExecutableChain).status ==
                      nsv_patch::PlanStatus::kUnsafeUpdateCombatChain,
                  "non-executable UpdateCombat chain is rejected");

  auto badApplied = applied;
  badApplied.storedOriginal = kNsvBase + nsv_patch::kUpdateCombatThunkRva;
  passed &= Check(nsv_patch::MakePatchPlan(badApplied).status ==
                      nsv_patch::PlanStatus::kUnsafeUpdateCombatChain,
                  "recursive already-applied chain is rejected");

  auto nullApplied = applied;
  nullApplied.storedOriginal = 0;
  nullApplied.storedOriginalExecutable = false;
  passed &= Check(nsv_patch::MakePatchPlan(nullApplied).status ==
                      nsv_patch::PlanStatus::kUnsafeUpdateCombatChain,
                  "null already-applied chain is rejected");

  auto aliasedApplied = applied;
  aliasedApplied.storedOriginal =
      kSkyrimBase + nsv_patch::kSkyrimGetAlphaRva;
  passed &= Check(nsv_patch::MakePatchPlan(aliasedApplied).status ==
                      nsv_patch::PlanStatus::kUnsafeUpdateCombatChain,
                  "GetAlpha-aliased already-applied chain is rejected");

  auto nonExecutableApplied = applied;
  nonExecutableApplied.storedOriginalExecutable = false;
  passed &= Check(nsv_patch::MakePatchPlan(nonExecutableApplied).status ==
                      nsv_patch::PlanStatus::kUnsafeUpdateCombatChain,
                  "non-executable already-applied chain is rejected");

  passed &=
      Check(nsv_patch::MatchesExpectedDllHash(nsv_patch::kExpectedDllSha256),
            "exact NPC Spell Variance digest is accepted");
  auto wrongHash = nsv_patch::kExpectedDllSha256;
  wrongHash[0] ^= 0xFF;
  passed &= Check(!nsv_patch::MatchesExpectedDllHash(wrongHash),
                  "altered NPC Spell Variance digest is rejected");

  const auto plan = nsv_patch::MakePatchPlan(valid);
  FakeBackend success(valid);
  const auto committed = nsv_patch::ApplyPatch(success, valid, plan);
  passed &=
      Check(committed.status == nsv_patch::ApplyStatus::kCommitted &&
                committed.pointersMatchPlan && committed.protectionsRestored,
            "stable pointer state commits and restores protections");
  passed &= Check(
      success.exchangeOrder ==
          std::vector{nsv_patch::PointerLocation::kGetAlpha,
                      nsv_patch::PointerLocation::kStoredOriginal,
                      nsv_patch::PointerLocation::kUpdateCombat},
      "publication order makes the thunk unreachable before changing its "
      "chain and publishes it last");
  passed &= Check(success.restoreStorageCalls == 1 &&
                      success.restoreVtableCalls == 1,
                  "successful publication restores both pages once");

  FakeBackend stateDrift(valid);
  ++stateDrift.values[Index(nsv_patch::PointerLocation::kUpdateCombat)];
  const auto drifted = nsv_patch::ApplyPatch(stateDrift, valid, plan);
  passed &= Check(
      drifted.status == nsv_patch::ApplyStatus::kStateChanged &&
          stateDrift.exchangeOrder.empty() &&
          stateDrift.restoreStorageCalls == 1 &&
          stateDrift.restoreVtableCalls == 1,
      "pre-publication pointer drift is rejected without publication");

  constexpr auto newerWrapper = kSkyrimBase + 0x765430;
  FakeBackend changedGetAlpha(valid);
  changedGetAlpha.beforeExchangeMutation = {
      true, nsv_patch::PointerLocation::kGetAlpha, 1,
      nsv_patch::PointerLocation::kGetAlpha, newerWrapper};
  const auto getAlphaLoss =
      nsv_patch::ApplyPatch(changedGetAlpha, valid, plan);
  passed &= Check(
      getAlphaLoss.status == nsv_patch::ApplyStatus::kPublishIncomplete &&
          changedGetAlpha
                  .values[Index(nsv_patch::PointerLocation::kGetAlpha)] ==
              newerWrapper &&
          changedGetAlpha.exchangeOrder ==
              std::vector{nsv_patch::PointerLocation::kGetAlpha},
      "real GetAlpha ownership loss is preserved without later writes");

  constexpr auto competingOriginal = kSkyrimBase + 0x765480;
  FakeBackend changedOriginal(valid);
  changedOriginal.beforeExchangeMutation = {
      true, nsv_patch::PointerLocation::kStoredOriginal, 1,
      nsv_patch::PointerLocation::kStoredOriginal, competingOriginal};
  const auto originalLoss =
      nsv_patch::ApplyPatch(changedOriginal, valid, plan);
  passed &= Check(
      originalLoss.status == nsv_patch::ApplyStatus::kPublishIncomplete &&
          changedOriginal
                  .values[Index(nsv_patch::PointerLocation::kGetAlpha)] ==
              plan.getAlpha &&
          changedOriginal
                  .values[Index(nsv_patch::PointerLocation::kUpdateCombat)] ==
              valid.currentUpdateCombat &&
          changedOriginal
                  .values[Index(nsv_patch::PointerLocation::kStoredOriginal)] ==
              competingOriginal,
      "real stored-original ownership loss leaves the donor thunk unreachable");
  passed &= Check(
      changedOriginal.exchangeOrder ==
          std::vector{nsv_patch::PointerLocation::kGetAlpha,
                      nsv_patch::PointerLocation::kStoredOriginal},
      "stored-original ownership loss stops before UpdateCombat publication");

  FakeBackend lateOriginalDrift(valid);
  lateOriginalDrift.exchangeMutation = {
      true, nsv_patch::PointerLocation::kStoredOriginal, 1,
      nsv_patch::PointerLocation::kStoredOriginal,
      valid.storedOriginal};
  const auto lateOriginalResult =
      nsv_patch::ApplyPatch(lateOriginalDrift, valid, plan);
  passed &= Check(
      lateOriginalResult.status ==
              nsv_patch::ApplyStatus::kPublishIncomplete &&
          lateOriginalDrift
                  .values[Index(nsv_patch::PointerLocation::kGetAlpha)] ==
              plan.getAlpha &&
          lateOriginalDrift
                  .values[Index(nsv_patch::PointerLocation::kUpdateCombat)] ==
              valid.currentUpdateCombat &&
          lateOriginalDrift.exchangeOrder ==
              std::vector{nsv_patch::PointerLocation::kGetAlpha,
                          nsv_patch::PointerLocation::kStoredOriginal},
      "late stored-original drift prevents UpdateCombat publication");

  FakeBackend lateGetAlphaDrift(valid);
  lateGetAlphaDrift.exchangeMutation = {
      true, nsv_patch::PointerLocation::kStoredOriginal, 1,
      nsv_patch::PointerLocation::kGetAlpha, newerWrapper};
  const auto lateGetAlphaResult =
      nsv_patch::ApplyPatch(lateGetAlphaDrift, valid, plan);
  passed &= Check(
      lateGetAlphaResult.status ==
              nsv_patch::ApplyStatus::kPublishIncomplete &&
          lateGetAlphaDrift
                  .values[Index(nsv_patch::PointerLocation::kGetAlpha)] ==
              newerWrapper &&
          lateGetAlphaDrift
                  .values[Index(nsv_patch::PointerLocation::kUpdateCombat)] ==
              valid.currentUpdateCombat &&
          lateGetAlphaDrift.exchangeOrder ==
              std::vector{nsv_patch::PointerLocation::kGetAlpha,
                          nsv_patch::PointerLocation::kStoredOriginal},
      "late GetAlpha drift prevents UpdateCombat publication");

  FakeBackend changedUpdateCombat(valid);
  changedUpdateCombat.beforeExchangeMutation = {
      true, nsv_patch::PointerLocation::kUpdateCombat, 1,
      nsv_patch::PointerLocation::kUpdateCombat, newerWrapper};
  const auto updateCombatLoss =
      nsv_patch::ApplyPatch(changedUpdateCombat, valid, plan);
  passed &= Check(
      updateCombatLoss.status ==
              nsv_patch::ApplyStatus::kPublishIncomplete &&
          changedUpdateCombat
                  .values[Index(nsv_patch::PointerLocation::kGetAlpha)] ==
              plan.getAlpha &&
          changedUpdateCombat
                  .values[Index(nsv_patch::PointerLocation::kUpdateCombat)] ==
              newerWrapper &&
          changedUpdateCombat
                  .values[Index(nsv_patch::PointerLocation::kStoredOriginal)] ==
              plan.storedOriginal &&
          changedUpdateCombat.compareExchangeCalls[Index(
              nsv_patch::PointerLocation::kGetAlpha)] == 1 &&
          changedUpdateCombat.compareExchangeCalls[Index(
              nsv_patch::PointerLocation::kStoredOriginal)] == 1 &&
          changedUpdateCombat.compareExchangeCalls[Index(
              nsv_patch::PointerLocation::kUpdateCombat)] == 1,
      "real UpdateCombat ownership loss preserves the monotonic partial state");

  FakeBackend vtableProtectFailure(valid);
  vtableProtectFailure.makeVtableWritable = false;
  passed &=
      Check(nsv_patch::ApplyPatch(vtableProtectFailure, valid, plan).status ==
                nsv_patch::ApplyStatus::kProtectionPreparationFailed,
            "vtable protection failure leaves state unchanged");

  FakeBackend vtablePreparationDrift(valid);
  vtablePreparationDrift.makeVtableWritable = false;
  vtablePreparationDrift.mutateDuringVtablePreparation = true;
  vtablePreparationDrift.preparationMutationValue = newerWrapper;
  const auto vtableDriftResult =
      nsv_patch::ApplyPatch(vtablePreparationDrift, valid, plan);
  passed &= Check(
      vtableDriftResult.status ==
              nsv_patch::ApplyStatus::kProtectionPreparationFailed &&
          !vtableDriftResult.pointersMatchObserved,
      "vtable preparation failure does not claim unverified pointer equality");

  FakeBackend storageProtectFailure(valid);
  storageProtectFailure.makeStorageWritable = false;
  passed &=
      Check(nsv_patch::ApplyPatch(storageProtectFailure, valid, plan).status ==
                nsv_patch::ApplyStatus::kProtectionPreparationFailed,
            "storage protection failure restores vtable protection");

  FakeBackend storagePreparationDrift(valid);
  storagePreparationDrift.makeStorageWritable = false;
  storagePreparationDrift.mutateDuringStoragePreparation = true;
  storagePreparationDrift.preparationMutationValue = newerWrapper;
  const auto storageDriftResult =
      nsv_patch::ApplyPatch(storagePreparationDrift, valid, plan);
  passed &= Check(
      storageDriftResult.status ==
              nsv_patch::ApplyStatus::kProtectionPreparationFailed &&
          !storageDriftResult.pointersMatchObserved &&
          storagePreparationDrift.restoreVtableCalls == 1,
      "storage preparation failure reports pointer drift and restores the "
      "vtable protection");

  FakeBackend storageFailureRestoreDrift(valid);
  storageFailureRestoreDrift.makeStorageWritable = false;
  storageFailureRestoreDrift.mutateDuringVtableRestore = true;
  storageFailureRestoreDrift.preparationMutationValue = newerWrapper;
  const auto storageFailureRestoreDriftResult =
      nsv_patch::ApplyPatch(storageFailureRestoreDrift, valid, plan);
  passed &= Check(
      storageFailureRestoreDriftResult.status ==
              nsv_patch::ApplyStatus::kProtectionPreparationFailed &&
          !storageFailureRestoreDriftResult.pointersMatchObserved,
      "preparation result snapshots pointers after vtable restoration");

  FakeBackend failedPreparationRestore(valid);
  failedPreparationRestore.makeStorageWritable = false;
  failedPreparationRestore.restoreVtable = false;
  passed &= Check(
      nsv_patch::ApplyPatch(failedPreparationRestore, valid, plan).status ==
          nsv_patch::ApplyStatus::kProtectionRestoreFailed,
      "preparation restoration failure is reported");

  FakeBackend failedCommitRestore(valid);
  failedCommitRestore.restoreStorage = false;
  const auto commitRestore =
      nsv_patch::ApplyPatch(failedCommitRestore, valid, plan);
  passed &=
      Check(commitRestore.status ==
                    nsv_patch::ApplyStatus::kProtectionRestoreFailed &&
                commitRestore.pointersMatchPlan &&
                failedCommitRestore.restoreStorageCalls == 1 &&
                failedCommitRestore.restoreVtableCalls == 1,
            "committed state with protection failure is reported accurately");

  FakeBackend commitRestoreDrift(valid);
  commitRestoreDrift.mutateDuringStorageRestore = true;
  commitRestoreDrift.preparationMutationValue = newerWrapper;
  const auto commitRestoreDriftResult =
      nsv_patch::ApplyPatch(commitRestoreDrift, valid, plan);
  passed &= Check(
      commitRestoreDriftResult.status ==
              nsv_patch::ApplyStatus::kPublishIncomplete &&
          !commitRestoreDriftResult.pointersMatchPlan &&
          commitRestoreDriftResult.protectionsRestored,
      "commit result snapshots pointer drift after protection restoration");

  FakeBackend bothRestoreFailures(valid);
  bothRestoreFailures.restoreStorage = false;
  bothRestoreFailures.restoreVtable = false;
  const auto bothRestoreResult =
      nsv_patch::ApplyPatch(bothRestoreFailures, valid, plan);
  passed &= Check(
      bothRestoreResult.status ==
              nsv_patch::ApplyStatus::kProtectionRestoreFailed &&
          !bothRestoreResult.protectionsRestored &&
          bothRestoreFailures.restoreStorageCalls == 1 &&
          bothRestoreFailures.restoreVtableCalls == 1,
      "both protection restoration failures are attempted and reported");

  FakeBackend compoundRecoveryFailure(valid);
  compoundRecoveryFailure.beforeExchangeMutation = {
      true, nsv_patch::PointerLocation::kUpdateCombat, 1,
      nsv_patch::PointerLocation::kUpdateCombat, newerWrapper};
  compoundRecoveryFailure.restoreStorage = false;
  const auto compoundRecovery =
      nsv_patch::ApplyPatch(compoundRecoveryFailure, valid, plan);
  passed &= Check(
      compoundRecovery.status ==
              nsv_patch::ApplyStatus::kProtectionRestoreFailed &&
          !compoundRecovery.protectionsRestored &&
          !compoundRecovery.pointersMatchPlan &&
          !compoundRecovery.pointersMatchObserved &&
          compoundRecoveryFailure.restoreStorageCalls == 1 &&
          compoundRecoveryFailure.restoreVtableCalls == 1,
      "incomplete publication and protection recovery retain both facts");

  return passed ? 0 : 1;
}
