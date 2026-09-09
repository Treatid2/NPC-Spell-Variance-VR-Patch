#include "AtomicPointerRead.h"

#include <Windows.h>

#include <cstdint>
#include <iostream>

namespace {

constexpr std::uintptr_t kExpected = 0x123456789ABCDEF0ULL;

} // namespace

int main() {
  auto *const page = VirtualAlloc(nullptr, 4096, MEM_COMMIT | MEM_RESERVE,
                                  PAGE_READWRITE);
  if (!page) {
    std::cerr << "FAILED: unable to allocate test page\n";
    return 1;
  }

  auto *const pointer = static_cast<std::uintptr_t *>(page);
  *pointer = kExpected;

  DWORD previousProtection = 0;
  if (!VirtualProtect(page, 4096, PAGE_READONLY, &previousProtection)) {
    std::cerr << "FAILED: unable to make test page read-only\n";
    VirtualFree(page, 0, MEM_RELEASE);
    return 1;
  }

  const auto observed = nsv_patch::AtomicLoadPointer(pointer);

  DWORD ignored = 0;
  const bool protectionRestored =
      VirtualProtect(page, 4096, previousProtection, &ignored) != FALSE;
  const bool released = VirtualFree(page, 0, MEM_RELEASE) != FALSE;

  if (observed != kExpected) {
    std::cerr << "FAILED: atomic load returned the wrong pointer\n";
    return 1;
  }
  if (!protectionRestored || !released) {
    std::cerr << "FAILED: unable to clean up test page\n";
    return 1;
  }
  return 0;
}
