// Copyright (C) 2024 Intel Corporation
// Under the Apache License v2.0 with LLVM Exceptions. See LICENSE.TXT.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include <umf/memspace.h>

#include "memspace_fixtures.hpp"
#include "memspace_helpers.hpp"
#include "memspace_internal.h"
#include "test_helpers.h"

/*

canQueryLatency is used in a parameter generator as a functional pointer
 https://man.archlinux.org/man/hwloc_memattr_get_value.3.en
 0 on success - hwloc_memattr_get_value
 assume overall failure
expect instead of add_failure: more verbosity
*/

static bool canQueryLatency(size_t nodeId) {
    hwloc_topology_t topology = nullptr;
    int ret = hwloc_topology_init(&topology);
    // UT_ASSERTeq(ret, 0);
    // EXPECT_NE(ret, 0);
    // if (ret == 0) {
    //     //ADD_FAILURE();
    // //    EXPECT_NE(ret, 0);
    //     std::cerr << "stderr ret is not 0 at line: " << __LINE__ << " in file: " << __FILE__ << std::endl;
    //     GTEST_OUT_EQ

    //     return false;
    // }
    if (!GTEST_OUT_EQ(ret, 0)) {
        // EXPECT_NE(ret, 0);
        return false;
    }
    ret = hwloc_topology_load(topology);
    //UT_ASSERTeq(ret, 0);
    if (!GTEST_OUT_EQ(ret, 0)) {
        return false;
    }

    hwloc_obj_t numaNode =
        hwloc_get_obj_by_type(topology, HWLOC_OBJ_NUMANODE, nodeId);
    //UT_ASSERTne(numaNode, nullptr);
    if (!GTEST_OUT_NE(numaNode, nullptr)) {
        return false;
    }

    // Setup initiator structure.
    struct hwloc_location initiator;
    initiator.location.cpuset = numaNode->cpuset;
    initiator.type = hwloc_location_type_alias::HWLOC_LOCATION_TYPE_CPUSET;

    hwloc_uint64_t value = 0;
    ret = hwloc_memattr_get_value(topology, HWLOC_MEMATTR_ID_LATENCY, numaNode,
                                  &initiator, 0, &value);

    hwloc_topology_destroy(topology);

    if (!GTEST_OUT_EQ(ret, 0)) {
        return false;
    }
    else {
        return true;
    }
    // return (ret == 0);
}

INSTANTIATE_TEST_SUITE_P(memspaceLowestLatencyTest, memspaceGetTest,
                         ::testing::Values(memspaceGetParams{
                             canQueryLatency, umfMemspaceLowestLatencyGet}));

INSTANTIATE_TEST_SUITE_P(memspaceLowestLatencyProviderTest,
                         memspaceProviderTest,
                         ::testing::Values(memspaceGetParams{
                             canQueryLatency, umfMemspaceLowestLatencyGet}));
