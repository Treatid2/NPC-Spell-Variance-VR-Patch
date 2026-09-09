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
  std::array<int, 3> failCompareExchangeCall{};
  std::vector<nsv_patch::PointerLocation> exchangeOrder{};
  ExchangeMutation exchangeMutation{};
  bool makeVtableWritable = true;
  bool makeStorageWritable = true;
  bool restoreVtable = true;
  bool restoreStorage = true;
  bool mutateDuringVtablePreparation = false;
  bool mutateDuringStoragePreparation = false;
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
    return restoreVtable;
  }
  bool RestoreStorageProtection() {
    ++restoreStorageCalls;
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
    const bool succeeded =
        failCompareExchangeCall[index] != compareExchangeCalls[index] &&
        values[index] == a_expected;
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

  FakeBackend stateDrift(valid);
  ++stateDrift.values[Index(nsv_patch::PointerLocation::kUpdateCombat)];
  const auto drifted = nsv_patch::ApplyPatch(stateDrift, valid, plan);
  passed &= Check(drifted.status == nsv_patch::ApplyStatus::kStateChanged,
                  "pre-publication pointer drift is rejected");

  for (const auto failureLocation :
       {nsv_patch::PointerLocation::kStoredOriginal,
        nsv_patch::PointerLocation::kUpdateCombat,
        nsv_patch::PointerLocation::kGetAlpha}) {
    FakeBackend publishFailure(valid);
    publishFailure.failCompareExchangeCall[Index(failureLocation)] = 1;
    const auto result = nsv_patch::ApplyPatch(publishFailure, valid, plan);
    passed &= Check(
        result.status == nsv_patch::ApplyStatus::kPublishFailedRolledBack &&
            result.pointersMatchObserved && result.protectionsRestored,
        "publication failure rolls back and restores protections");
  }

  FakeBackend failedRecovery(valid);
  failedRecovery.failCompareExchangeCall[Index(
      nsv_patch::PointerLocation::kUpdateCombat)] = 1;
  failedRecovery
      .failCompareExchangeCall[Index(nsv_patch::PointerLocation::kGetAlpha)] =
      2;
  const auto recovery = nsv_patch::ApplyPatch(failedRecovery, valid, plan);
  passed &=
      Check(recovery.status == nsv_patch::ApplyStatus::kRecoveryIncomplete,
            "contested rollback reports incomplete recovery");

  constexpr auto newerWrapper = kSkyrimBase + 0x765430;
  FakeBackend newerE6Writer(valid);
  newerE6Writer.exchangeMutation = {
      true, nsv_patch::PointerLocation::kUpdateCombat, 1,
      nsv_patch::PointerLocation::kUpdateCombat, newerWrapper};
  const auto newerE6Recovery =
      nsv_patch::ApplyPatch(newerE6Writer, valid, plan);
  passed &= Check(
      newerE6Recovery.status ==
              nsv_patch::ApplyStatus::kRecoveryIncomplete &&
          newerE6Writer.values[Index(nsv_patch::PointerLocation::kGetAlpha)] ==
              plan.getAlpha &&
          newerE6Writer
                  .values[Index(nsv_patch::PointerLocation::kUpdateCombat)] ==
              newerWrapper &&
          newerE6Writer
                  .values[Index(nsv_patch::PointerLocation::kStoredOriginal)] ==
              plan.storedOriginal,
      "newer E6 writer keeps the corrected thunk chain intact");

  constexpr auto competingOriginal = kSkyrimBase + 0x765480;
  FakeBackend changedOriginal(valid);
  changedOriginal.exchangeMutation = {
      true, nsv_patch::PointerLocation::kGetAlpha, 1,
      nsv_patch::PointerLocation::kStoredOriginal, competingOriginal};
  const auto changedOriginalRecovery =
      nsv_patch::ApplyPatch(changedOriginal, valid, plan);
  passed &= Check(
      changedOriginalRecovery.status ==
              nsv_patch::ApplyStatus::kRecoveryIncomplete &&
          changedOriginal
                  .values[Index(nsv_patch::PointerLocation::kGetAlpha)] ==
              plan.getAlpha &&
          changedOriginal
                  .values[Index(nsv_patch::PointerLocation::kUpdateCombat)] ==
              valid.currentUpdateCombat &&
          changedOriginal
                  .values[Index(nsv_patch::PointerLocation::kStoredOriginal)] ==
              competingOriginal,
      "changed stored original is not exposed through the GetAlpha slot");

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

  FakeBackend failedRollbackRestore(valid);
  failedRollbackRestore.failCompareExchangeCall[Index(
      nsv_patch::PointerLocation::kUpdateCombat)] = 1;
  failedRollbackRestore.restoreVtable = false;
  const auto rollbackRestore =
      nsv_patch::ApplyPatch(failedRollbackRestore, valid, plan);
  passed &=
      Check(rollbackRestore.status ==
                    nsv_patch::ApplyStatus::kProtectionRestoreFailed &&
                rollbackRestore.pointersMatchObserved &&
                failedRollbackRestore.restoreStorageCalls == 1 &&
                failedRollbackRestore.restoreVtableCalls == 1,
            "rolled-back state with protection failure is reported accurately");

  FakeBackend compoundRecoveryFailure(valid);
  compoundRecoveryFailure.exchangeMutation = {
      true, nsv_patch::PointerLocation::kUpdateCombat, 1,
      nsv_patch::PointerLocation::kUpdateCombat, newerWrapper};
  compoundRecoveryFailure.restoreStorage = false;
  const auto compoundRecovery =
      nsv_patch::ApplyPatch(compoundRecoveryFailure, valid, plan);
  passed &= Check(
      compoundRecovery.status == nsv_patch::ApplyStatus::kRecoveryIncomplete &&
          !compoundRecovery.protectionsRestored &&
          compoundRecoveryFailure.restoreStorageCalls == 1 &&
          compoundRecoveryFailure.restoreVtableCalls == 1,
      "incomplete pointer and protection recovery retain both failure facts");

  return passed ? 0 : 1;
}
