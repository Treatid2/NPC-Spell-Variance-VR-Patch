#pragma once

#include <atomic>
#include <exception>
#include <functional>
#include <utility>

namespace nsv_patch {

template <class Installer, class FailureHandler>
void HandlePostLoadInstallation(bool a_isPostLoad, std::atomic_bool &a_attempted,
                                Installer &&a_installer,
                                FailureHandler &&a_failureHandler) {
  if (!a_isPostLoad || a_attempted.exchange(true)) {
    return;
  }

  if (!std::invoke(std::forward<Installer>(a_installer))) {
    std::invoke(std::forward<FailureHandler>(a_failureHandler));
    // A misconfigured failure handler must never return control to gameplay.
    std::terminate();
  }
}

} // namespace nsv_patch
