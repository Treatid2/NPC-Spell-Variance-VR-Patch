#include "AtomicPointerRead.h"
#include "BinaryIdentity.h"
#include "PatchPlan.h"
#include "PatchTransaction.h"

namespace {

constexpr std::wstring_view kNsvModuleName = L"NPCSpellVariance.dll";
constexpr std::array<std::uint8_t, 16> kExpectedThunkEntry{
    0x40, 0x57, 0x48, 0x83, 0xEC, 0x40, 0x48, 0x8B,
    0xF9, 0xFF, 0x15, 0xF1, 0x8C, 0x08, 0x00, 0x48,
};

std::atomic_bool g_attempted{false};

[[nodiscard]] const char *
DescribeStatus(nsv_patch::PlanStatus a_status) noexcept {
  switch (a_status) {
  case nsv_patch::PlanStatus::kReady:
    return "ready";
  case nsv_patch::PlanStatus::kUnsupportedBuild:
    return "unsupported NPC Spell Variance build";
  case nsv_patch::PlanStatus::kUnexpectedGetAlphaHook:
    return "GetAlpha is not hooked by the expected NPC Spell Variance thunk";
  case nsv_patch::PlanStatus::kUnexpectedOriginalGetAlpha:
    return "NPC Spell Variance did not preserve the expected VR GetAlpha";
  case nsv_patch::PlanStatus::kUnsafeUpdateCombatChain:
    return "UpdateCombat chain target is null, recursive, aliased, or not "
           "executable";
  case nsv_patch::PlanStatus::kAlreadyApplied:
    return "already applied";
  }
  return "unknown";
}

[[nodiscard]] bool IsReadableRange(std::uintptr_t a_address,
                                   std::size_t a_size) noexcept {
  if (a_address == 0 || a_size == 0 ||
      a_address > (std::numeric_limits<std::uintptr_t>::max)() - a_size) {
    return false;
  }

  const auto end = a_address + a_size;
  auto cursor = a_address;
  while (cursor < end) {
    MEMORY_BASIC_INFORMATION region{};
    if (VirtualQuery(reinterpret_cast<const void *>(cursor), &region,
                     sizeof(region)) != sizeof(region) ||
        region.State != MEM_COMMIT ||
        (region.Protect & (PAGE_GUARD | PAGE_NOACCESS)) != 0) {
      return false;
    }

    const auto regionBase =
        reinterpret_cast<std::uintptr_t>(region.BaseAddress);
    if (regionBase >
        (std::numeric_limits<std::uintptr_t>::max)() - region.RegionSize) {
      return false;
    }
    const auto regionEnd = regionBase + region.RegionSize;
    if (regionEnd <= cursor) {
      return false;
    }
    cursor = (std::min)(end, regionEnd);
  }
  return true;
}

[[nodiscard]] bool IsExecutableAddress(std::uintptr_t a_address) noexcept {
  MEMORY_BASIC_INFORMATION region{};
  if (a_address == 0 ||
      VirtualQuery(reinterpret_cast<const void *>(a_address), &region,
                   sizeof(region)) != sizeof(region) ||
      region.State != MEM_COMMIT ||
      (region.Protect & (PAGE_GUARD | PAGE_NOACCESS)) != 0) {
    return false;
  }

  constexpr DWORD kExecutable = PAGE_EXECUTE | PAGE_EXECUTE_READ |
                                PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY;
  return (region.Protect & kExecutable) != 0;
}

[[nodiscard]] bool
ReadModuleFileHash(HMODULE a_module,
                   std::array<std::uint8_t, 32> &a_digest) noexcept {
  std::wstring path(32768, L'\0');
  const auto pathLength = GetModuleFileNameW(a_module, path.data(),
                                             static_cast<DWORD>(path.size()));
  if (pathLength == 0 || pathLength >= path.size()) {
    return false;
  }
  path.resize(pathLength);

  const HANDLE file = CreateFileW(
      path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_DELETE, nullptr,
      OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (file == INVALID_HANDLE_VALUE) {
    return false;
  }

  LARGE_INTEGER fileSize{};
  constexpr LONGLONG kMaximumSupportedFileSize = 16 * 1024 * 1024;
  if (!GetFileSizeEx(file, &fileSize) || fileSize.QuadPart <= 0 ||
      fileSize.QuadPart > kMaximumSupportedFileSize) {
    CloseHandle(file);
    return false;
  }

  BCRYPT_ALG_HANDLE algorithm = nullptr;
  BCRYPT_HASH_HANDLE hash = nullptr;
  std::vector<std::uint8_t> hashObject;
  bool succeeded = false;

  do {
    if (BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM,
                                    nullptr, 0) < 0) {
      break;
    }

    DWORD objectSize = 0;
    DWORD resultSize = 0;
    if (BCryptGetProperty(algorithm, BCRYPT_OBJECT_LENGTH,
                          reinterpret_cast<PUCHAR>(&objectSize),
                          sizeof(objectSize), &resultSize, 0) < 0 ||
        resultSize != sizeof(objectSize) || objectSize == 0) {
      break;
    }
    hashObject.resize(objectSize);

    if (BCryptCreateHash(algorithm, &hash, hashObject.data(), objectSize,
                         nullptr, 0, 0) < 0) {
      break;
    }

    std::array<std::uint8_t, 64 * 1024> buffer{};
    for (;;) {
      DWORD bytesRead = 0;
      if (!ReadFile(file, buffer.data(), static_cast<DWORD>(buffer.size()),
                    &bytesRead, nullptr)) {
        break;
      }
      if (bytesRead == 0) {
        succeeded =
            BCryptFinishHash(hash, a_digest.data(),
                             static_cast<ULONG>(a_digest.size()), 0) >= 0;
        break;
      }
      if (BCryptHashData(hash, buffer.data(), bytesRead, 0) < 0) {
        break;
      }
    }
  } while (false);

  if (hash) {
    BCryptDestroyHash(hash);
  }
  if (algorithm) {
    BCryptCloseAlgorithmProvider(algorithm, 0);
  }
  CloseHandle(file);
  return succeeded;
}

[[nodiscard]] bool ReadImageIdentity(HMODULE a_module,
                                     std::uint32_t &a_timestamp,
                                     std::uint32_t &a_imageSize) noexcept {
  if (!a_module) {
    return false;
  }

  const auto base = reinterpret_cast<std::uintptr_t>(a_module);
  if (!IsReadableRange(base, sizeof(IMAGE_DOS_HEADER))) {
    return false;
  }
  const auto *dos = reinterpret_cast<const IMAGE_DOS_HEADER *>(base);
  if (dos->e_magic != IMAGE_DOS_SIGNATURE || dos->e_lfanew <= 0 ||
      static_cast<std::size_t>(dos->e_lfanew) >
          nsv_patch::kExpectedImageSize - sizeof(IMAGE_NT_HEADERS64)) {
    return false;
  }

  const auto ntAddress = base + static_cast<std::uintptr_t>(dos->e_lfanew);
  if (!IsReadableRange(ntAddress, sizeof(IMAGE_NT_HEADERS64))) {
    return false;
  }
  const auto *nt = reinterpret_cast<const IMAGE_NT_HEADERS64 *>(ntAddress);
  if (nt->Signature != IMAGE_NT_SIGNATURE ||
      nt->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC) {
    return false;
  }

  a_timestamp = nt->FileHeader.TimeDateStamp;
  a_imageSize = nt->OptionalHeader.SizeOfImage;
  return true;
}

[[nodiscard]] bool MatchesThunkSignature(std::uintptr_t a_thunk) noexcept {
  return IsReadableRange(a_thunk, kExpectedThunkEntry.size()) &&
         std::equal(kExpectedThunkEntry.begin(), kExpectedThunkEntry.end(),
                    reinterpret_cast<const std::uint8_t *>(a_thunk));
}

class WindowsPatchBackend {
public:
  WindowsPatchBackend(std::uintptr_t *a_getAlphaSlot,
                      std::uintptr_t *a_updateCombatSlot,
                      std::uintptr_t *a_originalStorage) noexcept
      : getAlphaSlot_(a_getAlphaSlot), updateCombatSlot_(a_updateCombatSlot),
        originalStorage_(a_originalStorage) {}

  [[nodiscard]] bool MakeVtableWritable() noexcept {
    constexpr auto slotSpan =
        (nsv_patch::kVrUpdateCombatSlot - nsv_patch::kSseUpdateCombatSlot + 1) *
        sizeof(std::uintptr_t);
    SYSTEM_INFO systemInfo{};
    GetSystemInfo(&systemInfo);
    const auto pageSize = static_cast<std::uintptr_t>(systemInfo.dwPageSize);
    if (pageSize == 0) {
      return false;
    }
    const auto firstPage =
        reinterpret_cast<std::uintptr_t>(getAlphaSlot_) / pageSize;
    const auto lastPage =
        (reinterpret_cast<std::uintptr_t>(getAlphaSlot_) + slotSpan - 1) /
        pageSize;
    if (firstPage != lastPage) {
      return false;
    }
    vtableWritable_ = VirtualProtect(getAlphaSlot_, slotSpan, PAGE_READWRITE,
                                     &oldVtableProtection_) != FALSE;
    return vtableWritable_;
  }

  [[nodiscard]] bool MakeStorageWritable() noexcept {
    storageWritable_ =
        VirtualProtect(originalStorage_, sizeof(std::uintptr_t), PAGE_READWRITE,
                       &oldStorageProtection_) != FALSE;
    return storageWritable_;
  }

  [[nodiscard]] bool RestoreVtableProtection() noexcept {
    if (!vtableWritable_) {
      return true;
    }
    DWORD ignored = 0;
    const bool restored =
        VirtualProtect(getAlphaSlot_,
                       (nsv_patch::kVrUpdateCombatSlot -
                        nsv_patch::kSseUpdateCombatSlot + 1) *
                           sizeof(std::uintptr_t),
                       oldVtableProtection_, &ignored) != FALSE;
    if (restored) {
      vtableWritable_ = false;
    }
    return restored;
  }

  [[nodiscard]] bool RestoreStorageProtection() noexcept {
    if (!storageWritable_) {
      return true;
    }
    DWORD ignored = 0;
    const bool restored =
        VirtualProtect(originalStorage_, sizeof(std::uintptr_t),
                       oldStorageProtection_, &ignored) != FALSE;
    if (restored) {
      storageWritable_ = false;
    }
    return restored;
  }

  [[nodiscard]] std::uintptr_t
  Read(nsv_patch::PointerLocation a_location) const noexcept {
    return nsv_patch::AtomicLoadPointer(Resolve(a_location));
  }

  [[nodiscard]] bool CompareExchange(nsv_patch::PointerLocation a_location,
                                     std::uintptr_t a_expected,
                                     std::uintptr_t a_value) noexcept {
    auto *target = Resolve(a_location);
    const auto previous = InterlockedCompareExchangePointer(
        reinterpret_cast<void *volatile *>(target),
        reinterpret_cast<void *>(a_value),
        reinterpret_cast<void *>(a_expected));
    return reinterpret_cast<std::uintptr_t>(previous) == a_expected;
  }

private:
  [[nodiscard]] std::uintptr_t *
  Resolve(nsv_patch::PointerLocation a_location) const noexcept {
    switch (a_location) {
    case nsv_patch::PointerLocation::kGetAlpha:
      return getAlphaSlot_;
    case nsv_patch::PointerLocation::kUpdateCombat:
      return updateCombatSlot_;
    case nsv_patch::PointerLocation::kStoredOriginal:
      return originalStorage_;
    }
    return nullptr;
  }

  std::uintptr_t *getAlphaSlot_;
  std::uintptr_t *updateCombatSlot_;
  std::uintptr_t *originalStorage_;
  DWORD oldStorageProtection_ = 0;
  DWORD oldVtableProtection_ = 0;
  bool vtableWritable_ = false;
  bool storageWritable_ = false;
};

[[nodiscard]] bool InstallPatch() {
  if (!REL::Module::IsVR() ||
      REL::Module::get().version() != SKSE::RUNTIME_VR_1_4_15) {
    logger::critical("Unsupported runtime; Skyrim VR 1.4.15 is required.");
    return false;
  }

  const auto nsvModule = GetModuleHandleW(kNsvModuleName.data());
  if (!nsvModule) {
    logger::critical(
        "NPCSpellVariance.dll is not loaded; nothing was changed.");
    return false;
  }

  const auto nsvBase = reinterpret_cast<std::uintptr_t>(nsvModule);
  std::array<std::uint8_t, 32> dllHash{};
  if (!ReadModuleFileHash(nsvModule, dllHash)) {
    logger::critical("Unable to hash the loaded NPC Spell Variance DLL; "
                     "nothing was changed.");
    return false;
  }
  if (!nsv_patch::MatchesExpectedDllHash(dllHash)) {
    logger::critical("NPC Spell Variance DLL content hash does not match the "
                     "supported 2.7.0 release; nothing was changed.");
    return false;
  }

  std::uint32_t timestamp = 0;
  std::uint32_t imageSize = 0;
  if (!ReadImageIdentity(nsvModule, timestamp, imageSize)) {
    logger::critical("Unable to read the NPC Spell Variance PE identity.");
    return false;
  }
  if (timestamp != nsv_patch::kExpectedTimestamp ||
      imageSize != nsv_patch::kExpectedImageSize) {
    logger::critical(
        "Unsupported NPC Spell Variance build (timestamp {:08X}, image size "
        "{:X}); nothing was changed.",
        timestamp, imageSize);
    return false;
  }

  const auto expectedThunk = nsvBase + nsv_patch::kUpdateCombatThunkRva;
  if (!MatchesThunkSignature(expectedThunk)) {
    logger::critical("NPC Spell Variance thunk signature mismatch; refusing "
                     "to modify the vtable.");
    return false;
  }

  REL::Relocation<std::uintptr_t> characterVtable{RE::VTABLE_Character[0]};
  auto *const getAlphaSlot = reinterpret_cast<std::uintptr_t *>(
      characterVtable.address() +
      nsv_patch::kSseUpdateCombatSlot * sizeof(std::uintptr_t));
  auto *const updateCombatSlot = reinterpret_cast<std::uintptr_t *>(
      characterVtable.address() +
      nsv_patch::kVrUpdateCombatSlot * sizeof(std::uintptr_t));
  auto *const originalStorage = reinterpret_cast<std::uintptr_t *>(
      nsvBase + nsv_patch::kOriginalFunctionStorageRva);

  constexpr auto slotSpan =
      (nsv_patch::kVrUpdateCombatSlot - nsv_patch::kSseUpdateCombatSlot + 1) *
      sizeof(std::uintptr_t);
  if (!IsReadableRange(reinterpret_cast<std::uintptr_t>(getAlphaSlot),
                       slotSpan) ||
      !IsReadableRange(reinterpret_cast<std::uintptr_t>(originalStorage),
                       sizeof(std::uintptr_t))) {
    logger::critical("Required hook storage is not safely readable; nothing "
                     "was changed.");
    return false;
  }

  WindowsPatchBackend backend(getAlphaSlot, updateCombatSlot, originalStorage);
  const auto currentGetAlpha =
      backend.Read(nsv_patch::PointerLocation::kGetAlpha);
  const auto currentUpdateCombat =
      backend.Read(nsv_patch::PointerLocation::kUpdateCombat);
  const auto storedOriginal =
      backend.Read(nsv_patch::PointerLocation::kStoredOriginal);

  const nsv_patch::ObservedState observed{
      timestamp,
      imageSize,
      nsvBase,
      REL::Module::get().base(),
      currentGetAlpha,
      currentUpdateCombat,
      storedOriginal,
      IsExecutableAddress(currentUpdateCombat),
      IsExecutableAddress(storedOriginal),
  };
  const auto plan = nsv_patch::MakePatchPlan(observed);
  if (plan.status == nsv_patch::PlanStatus::kAlreadyApplied) {
    logger::info("NPC Spell Variance VR hook correction is already active.");
    return true;
  }
  if (plan.status != nsv_patch::PlanStatus::kReady) {
    logger::critical("Patch precondition failed: {}. Nothing was changed.",
                     DescribeStatus(plan.status));
    return false;
  }

  const auto result = nsv_patch::ApplyPatch(backend, observed, plan);
  switch (result.status) {
  case nsv_patch::ApplyStatus::kCommitted:
    break;
  case nsv_patch::ApplyStatus::kStateChanged:
    logger::critical("Hook state changed after validation; nothing was "
                     "overwritten.");
    return false;
  case nsv_patch::ApplyStatus::kProtectionPreparationFailed:
    logger::critical("Unable to prepare writable hook storage; pointer state "
                     "was unchanged and page protections were restored.");
    return false;
  case nsv_patch::ApplyStatus::kPublishFailedRolledBack:
    logger::critical("Conditional hook publication failed; verified original "
                     "pointer state and page protections were restored.");
    return false;
  case nsv_patch::ApplyStatus::kRecoveryIncomplete:
    logger::critical("Hook publication failed and another writer prevented a "
                     "complete rollback. Restart Skyrim before continuing; "
                     "the residual pointer state was not reported as safe.");
    return false;
  case nsv_patch::ApplyStatus::kProtectionRestoreFailed:
    if (result.pointersMatchPlan) {
      logger::critical("The hook correction is active, but one or more page "
                       "protections could not be restored. Restart Skyrim "
                       "before continuing.");
    } else if (result.pointersMatchObserved) {
      logger::critical("Pointer state is unchanged, but one or more page "
                       "protections could not be restored. Restart Skyrim "
                       "before continuing.");
    } else {
      logger::critical("Hook state and page protection recovery are "
                       "incomplete. Restart Skyrim before continuing.");
    }
    return false;
  }

  logger::info("Restored Character::GetAlpha at VR slot E4 and moved NPC "
               "Spell Variance UpdateCombat hook to VR slot E6.");
  return true;
}

void OnSKSEMessage(SKSE::MessagingInterface::Message *a_message) {
  if (a_message->type != SKSE::MessagingInterface::kPostLoad ||
      g_attempted.exchange(true)) {
    return;
  }
  static_cast<void>(InstallPatch());
}

} // namespace

SKSEPluginLoad(const SKSE::LoadInterface *a_skse) {
  SKSE::Init(a_skse);
  logger::init();
  logger::info("NPC Spell Variance VR Patch 1.0.1 loading.");

  auto *messaging = SKSE::GetMessagingInterface();
  if (!messaging || !messaging->RegisterListener(OnSKSEMessage)) {
    logger::critical("Unable to register the post-load hook correction.");
    return false;
  }
  return true;
}
