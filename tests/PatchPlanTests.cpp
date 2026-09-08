#include "PatchPlan.h"

#include <iostream>

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
  };
}
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
  passed &= Check(
      ready.getAlpha == kSkyrimBase + nsv_patch::kSkyrimGetAlphaRva,
      "GetAlpha restoration target");
  passed &= Check(
      ready.updateCombat == kNsvBase + nsv_patch::kUpdateCombatThunkRva,
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
  passed &= Check(nsv_patch::MakePatchPlan(applied).status ==
                      nsv_patch::PlanStatus::kAlreadyApplied,
                  "applied state is idempotent");

  return passed ? 0 : 1;
}
