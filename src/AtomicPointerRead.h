#pragma once

#include <atomic>
#include <cstdint>

namespace nsv_patch {

[[nodiscard]] inline std::uintptr_t
AtomicLoadPointer(std::uintptr_t *a_target) noexcept {
  static_assert(std::atomic_ref<std::uintptr_t>::required_alignment <=
                alignof(std::uintptr_t));
  // Vtable pages remain read-only until publication prepares them.
  return std::atomic_ref<std::uintptr_t>(*a_target).load(
      std::memory_order_acquire);
}

} // namespace nsv_patch
