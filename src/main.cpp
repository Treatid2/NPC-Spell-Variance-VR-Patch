#include "PatchPlan.h"

namespace {

constexpr std::wstring_view kNsvModuleName = L"NPCSpellVariance.dll";
constexpr std::array<std::uint8_t, 16> kExpectedThunkEntry{
    0x40, 0x57, 0x48, 0x83, 0xEC, 0x40, 0x48, 0x8B,
    0xF9, 0xFF, 0x15, 0xF1, 0x8C, 0x08, 0x00, 0x48,
};

std::atomic_bool g_attempted{false};

[[nodiscard]] const char *DescribeStatus(
    nsv_patch::PlanStatus a_status) noexcept {
  switch (a_status) {
  case nsv_patch::PlanStatus::kReady:
    return "ready";
  case nsv_patch::PlanStatus::kUnsupportedBuild:
    return "unsupported NPC Spell Variance build";
  case nsv_patch::PlanStatus::kUnexpectedGetAlphaHook:
    return "GetAlpha is not hooked by the expected NPC Spell Variance thunk";
  case nsv_patch::PlanStatus::kUnexpectedOriginalGetAlpha:
    return "NPC Spell Variance did not preserve the expected VR GetAlpha";
  case nsv_patch::PlanStatus::kAlreadyApplied:
    return "already applied";
  }
  return "unknown";
}

[[nodiscard]] bool ReadImageIdentity(
    HMODULE a_module, std::uint32_t &a_timestamp,
    std::uint32_t &a_imageSize) noexcept {
  if (!a_module) {
    return false;
  }

  const auto base = reinterpret_cast<std::uintptr_t>(a_module);
  const auto *dos = reinterpret_cast<const IMAGE_DOS_HEADER *>(base);
  if (dos->e_magic != IMAGE_DOS_SIGNATURE || dos->e_lfanew <= 0) {
    return false;
  }

  const auto *nt = reinterpret_cast<const IMAGE_NT_HEADERS64 *>(
      base + static_cast<std::uintptr_t>(dos->e_lfanew));
  if (nt->Signature != IMAGE_NT_SIGNATURE ||
      nt->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC) {
    return false;
  }

  a_timestamp = nt->FileHeader.TimeDateStamp;
  a_imageSize = nt->OptionalHeader.SizeOfImage;
  return true;
}

[[nodiscard]] bool MatchesThunkSignature(std::uintptr_t a_thunk) noexcept {
  return std::equal(kExpectedThunkEntry.begin(), kExpectedThunkEntry.end(),
                    reinterpret_cast<const std::uint8_t *>(a_thunk));
}

[[nodiscard]] bool ApplyTransaction(std::uintptr_t *a_getAlphaSlot,
                                    std::uintptr_t *a_updateCombatSlot,
                                    std::uintptr_t *a_originalStorage,
                                    const nsv_patch::PatchPlan &a_plan,
                                    const nsv_patch::ObservedState &a_state) {
  DWORD oldVtableProtection = 0;
  DWORD oldStorageProtection = 0;
  constexpr auto slotSpan =
      (nsv_patch::kVrUpdateCombatSlot - nsv_patch::kSseUpdateCombatSlot + 1) *
      sizeof(std::uintptr_t);

  if (!VirtualProtect(a_getAlphaSlot, slotSpan, PAGE_READWRITE,
                      &oldVtableProtection)) {
    logger::critical("Unable to make the Character vtable writable ({}).",
                     GetLastError());
    return false;
  }
  if (!VirtualProtect(a_originalStorage, sizeof(std::uintptr_t), PAGE_READWRITE,
                      &oldStorageProtection)) {
    const auto error = GetLastError();
    DWORD ignored = 0;
    VirtualProtect(a_getAlphaSlot, slotSpan, oldVtableProtection, &ignored);
    logger::critical(
        "Unable to make NPC Spell Variance hook storage writable ({}).",
        error);
    return false;
  }

  const auto exchange = [](std::uintptr_t *a_target,
                           std::uintptr_t a_value) noexcept {
    InterlockedExchangePointer(
        reinterpret_cast<void *volatile *>(a_target),
        reinterpret_cast<void *>(a_value));
  };

  // Rebind the thunk's original before exposing it through the VR slot.
  exchange(a_originalStorage, a_plan.storedOriginal);
  exchange(a_updateCombatSlot, a_plan.updateCombat);
  exchange(a_getAlphaSlot, a_plan.getAlpha);

  const bool committed = *a_getAlphaSlot == a_plan.getAlpha &&
                         *a_updateCombatSlot == a_plan.updateCombat &&
                         *a_originalStorage == a_plan.storedOriginal;
  if (!committed) {
    exchange(a_originalStorage, a_state.storedOriginal);
    exchange(a_updateCombatSlot, a_state.currentUpdateCombat);
    exchange(a_getAlphaSlot, a_state.currentGetAlpha);
  }

  DWORD ignored = 0;
  const bool storageRestored =
      VirtualProtect(a_originalStorage, sizeof(std::uintptr_t),
                     oldStorageProtection, &ignored) != FALSE;
  const bool vtableRestored =
      VirtualProtect(a_getAlphaSlot, slotSpan, oldVtableProtection, &ignored) !=
      FALSE;

  if (!committed) {
    logger::critical("The vtable transaction did not commit; original values "
                     "were restored.");
    return false;
  }
  if (!storageRestored || !vtableRestored) {
    logger::critical("The hook was corrected, but page protection restoration "
                     "failed ({}).",
                     GetLastError());
    return false;
  }
  return true;
}

[[nodiscard]] bool InstallPatch() {
  if (!REL::Module::IsVR() ||
      REL::Module::get().version() != SKSE::RUNTIME_VR_1_4_15) {
    logger::critical("Unsupported runtime; Skyrim VR 1.4.15 is required.");
    return false;
  }

  const auto nsvModule = GetModuleHandleW(kNsvModuleName.data());
  if (!nsvModule) {
    logger::critical("NPCSpellVariance.dll is not loaded; nothing was changed.");
    return false;
  }

  const auto nsvBase = reinterpret_cast<std::uintptr_t>(nsvModule);
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

  const nsv_patch::ObservedState observed{
      timestamp,
      imageSize,
      nsvBase,
      REL::Module::get().base(),
      *getAlphaSlot,
      *updateCombatSlot,
      *originalStorage,
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

  if (!ApplyTransaction(getAlphaSlot, updateCombatSlot, originalStorage, plan,
                        observed)) {
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
  logger::info("NPC Spell Variance VR Patch 1.0.0 loading.");

  auto *messaging = SKSE::GetMessagingInterface();
  if (!messaging || !messaging->RegisterListener(OnSKSEMessage)) {
    logger::critical("Unable to register the post-load hook correction.");
    return false;
  }
  return true;
}
