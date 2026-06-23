/* Copyright (c) Advanced Micro Devices, Inc. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 */

#pragma once

namespace hipFile::test {

// Check AIS capability for tests that attempt to force fast path.
// Reimplements logic from hipfile/tools/ais-check/ais-check.
struct AisCapability {
    bool kernel_ais  = false; ///< AIS-init bit set on all GPU nodes in KFD topology
    bool hip_runtime = false; ///< hipAmdFileRead + hipAmdFileWrite resolvable
    bool amdgpu      = false; ///< kfd_ais_rw_file present in /proc/kallsyms

    bool fastpath_available() const
    {
        return kernel_ais && hip_runtime && amdgpu;
    }
};

AisCapability detectAisCapability();

bool fastpathAvailable();

}
