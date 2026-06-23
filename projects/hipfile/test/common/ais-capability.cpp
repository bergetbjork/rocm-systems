/* Copyright (c) Advanced Micro Devices, Inc. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 */

#include "ais-capability.h"

#include "hip.h"

#include <cstdint>
#include <fstream>
#include <iostream>
#include <string>
#include <zlib.h>

namespace hipFile::test {

namespace {

    // BIT6 of the KFD topology node "capability" field signals that amdgpu has
    // initialized AIS on that node, implying the kernel supports P2PDMA.
    constexpr uint64_t KFD_AIS_CAPABILITY_BIT = 0x40;

    bool kernelSupportsAis()
    {
        const std::string topology_nodes = "/sys/class/kfd/kfd/topology/nodes";

        bool any_gpu = false;

        for (int id = 0;; ++id) {
            const std::string props_path = topology_nodes + "/" + std::to_string(id) + "/properties";
            std::ifstream     in{props_path};
            if (!in.is_open()) {
                break;
            }

            uint64_t    capability = 0;
            uint32_t    simd_count = 0;
            std::string key;
            uint64_t    value;
            while (in >> key >> value) {
                if (key == "capability") {
                    capability = value;
                }
                else if (key == "simd_count") {
                    simd_count = static_cast<uint32_t>(value);
                }
            }

            if (simd_count == 0) {
                continue; // Not a GPU, disregard
            }
            any_gpu = true;
            if ((capability & KFD_AIS_CAPABILITY_BIT) == 0) {
                return false;
            }
        }

        return any_gpu;
    }

    bool hipRuntimeSupportsAis()
    {
        return hipFile::getHipAmdFileReadPtr() != nullptr && hipFile::getHipAmdFileWritePtr() != nullptr;
    }

    bool amdgpuSupportsAis()
    {
        std::ifstream kallsyms{"/proc/kallsyms"};
        if (!kallsyms.is_open()) {
            std::cerr << "Unable to open /proc/kallsyms\n";
            return false;
        }

        std::string line;
        while (std::getline(kallsyms, line)) {
            if (line.find("kfd_ais_rw_file") != std::string::npos) {
                return true;
            }
        }
        return false;
    }

}

// Reimplements logic from hipfile/tools/ais-check/ais-check.
AisCapability
detectAisCapability()
{
    AisCapability cap;
    cap.kernel_ais  = kernelSupportsAis();
    cap.hip_runtime = hipRuntimeSupportsAis();
    cap.amdgpu      = amdgpuSupportsAis();

    std::cerr << "AIS kernel AIS-init support: " << (cap.kernel_ais ? "yes" : "no") << "\n";
    std::cerr << "AIS HIP runtime support:     " << (cap.hip_runtime ? "yes" : "no") << "\n";
    std::cerr << "AIS amdgpu support:          " << (cap.amdgpu ? "yes" : "no") << "\n";

    return cap;
}

bool
fastpathAvailable()
{
    return detectAisCapability().fastpath_available();
}

}
