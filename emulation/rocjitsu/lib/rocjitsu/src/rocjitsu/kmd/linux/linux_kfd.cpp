// Copyright (c) 2026 Advanced Micro Devices, Inc.
// SPDX-License-Identifier: MIT

/// @file linux_kfd.cpp
/// @brief Shared Linux KFD ioctl helpers.

#include "rocjitsu/kmd/linux/linux_kfd.h"

#include "rocjitsu/base/rj_compiler.h"
RJ_DIAGNOSTIC_PUSH
RJ_DIAGNOSTIC_IGNORE_PEDANTIC
#include "linux/uapi/kfd_ioctl.h"
RJ_DIAGNOSTIC_POP

#include <cerrno>
#include <chrono>
#include <cstdint>

namespace rocjitsu {

int LinuxKfd::get_version_ioctl(void *arg) {
  auto *args = static_cast<kfd_ioctl_get_version_args *>(arg);
  if (!args) {
    errno = EINVAL;
    return -1;
  }
  args->major_version = KFD_IOCTL_MAJOR_VERSION;
  args->minor_version = KFD_IOCTL_MINOR_VERSION;
  return 0;
}

int LinuxKfd::get_clock_counters_ioctl(void *arg) {
  auto *args = static_cast<kfd_ioctl_get_clock_counters_args *>(arg);
  if (!args) {
    errno = EINVAL;
    return -1;
  }

  auto now = std::chrono::steady_clock::now().time_since_epoch();
  uint64_t ns =
      static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(now).count());
  args->system_clock_freq = 1000000000ULL;
  args->system_clock_counter = ns;
  args->cpu_clock_counter = ns;
  args->gpu_clock_counter = ns;
  return 0;
}

} // namespace rocjitsu
