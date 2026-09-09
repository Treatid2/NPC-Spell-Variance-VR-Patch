#pragma once

#include <cstddef>
#include <cstdint>

namespace nsv_patch {

inline constexpr std::uint32_t kExpectedTimestamp = 0x6A6A6035;
inline constexpr std::uint32_t kExpectedImageSize = 0xC6000;
inline constexpr std::uintptr_t kUpdateCombatThunkRva = 0x34840;
inline constexpr std::uintptr_t kOriginalFunctionStorageRva = 0xBD540;
inline constexpr std::uintptr_t kSkyrimGetAlphaRva = 0x638A30;
inline constexpr std::size_t kSseUpdateCombatSlot = 0xE4;
inline constexpr std::size_t kVrUpdateCombatSlot = 0xE6;

enum class PlanStatus {
  kReady,
  kUnsupportedBuild,
  kUnexpectedGetAlphaHook,
  kUnexpectedOriginalGetAlpha,
  kUnsafeUpdateCombatChain,
  kAlreadyApplied,
};

struct ObservedState {
  std::uint32_t timestamp;
  std::uint32_t imageSize;
  std::uintptr_t nsvBase;
  std::uintptr_t skyrimBase;
  std::uintptr_t currentGetAlpha;
  std::uintptr_t currentUpdateCombat;
  std::uintptr_t storedOriginal;
  bool currentUpdateCombatExecutable;
  bool storedOriginalExecutable;
};

struct PatchPlan {
  PlanStatus status;
  std::uintptr_t getAlpha;
  std::uintptr_t updateCombat;
  std::uintptr_t storedOriginal;
};

[[nodiscard]] constexpr PatchPlan
MakePatchPlan(const ObservedState &a_state) noexcept {
  const auto expectedThunk = a_state.nsvBase + kUpdateCombatThunkRva;
  const auto expectedGetAlpha = a_state.skyrimBase + kSkyrimGetAlphaRva;
  const auto isSafeChainTarget = [&](std::uintptr_t a_target,
                                     bool a_executable) {
    return a_executable && a_target != 0 && a_target != expectedThunk &&
           a_target != expectedGetAlpha;
  };

  if (a_state.timestamp != kExpectedTimestamp ||
      a_state.imageSize != kExpectedImageSize) {
    return {PlanStatus::kUnsupportedBuild, 0, 0, 0};
  }
  if (a_state.currentGetAlpha == expectedGetAlpha &&
      a_state.currentUpdateCombat == expectedThunk) {
    if (!isSafeChainTarget(a_state.storedOriginal,
                           a_state.storedOriginalExecutable)) {
      return {PlanStatus::kUnsafeUpdateCombatChain, 0, 0, 0};
    }
    return {PlanStatus::kAlreadyApplied, a_state.currentGetAlpha,
            a_state.currentUpdateCombat, a_state.storedOriginal};
  }
  if (a_state.currentGetAlpha != expectedThunk) {
    return {PlanStatus::kUnexpectedGetAlphaHook, 0, 0, 0};
  }
  if (a_state.storedOriginal != expectedGetAlpha) {
    return {PlanStatus::kUnexpectedOriginalGetAlpha, 0, 0, 0};
  }
  if (!isSafeChainTarget(a_state.currentUpdateCombat,
                         a_state.currentUpdateCombatExecutable)) {
    return {PlanStatus::kUnsafeUpdateCombatChain, 0, 0, 0};
  }

  return {PlanStatus::kReady, expectedGetAlpha, expectedThunk,
          a_state.currentUpdateCombat};
}

} // namespace nsv_patch
