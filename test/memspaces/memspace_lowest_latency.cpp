// Copyright (C) 2024 Intel Corporation
// Under the Apache License v2.0 with LLVM Exceptions. See LICENSE.TXT.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include <umf/memspace.h>

#include "memspace_fixtures.hpp"
#include "memspace_helpers.hpp"
#include "memspace_internal.h"
#include "test_helpers.h"

/*
canQueryLatency is used in a parameter generator as a functional pointer and is not a standalone test, thus it cannot contain GTEST asserts. EXPECT_* or ADD_FAILURE() do not cancel the execution of the code following after, but they interfere with GTEST_SKIP() in fixtures: the execution of the tests included in the suite assigned to the given fixture is not cancelled if EXPECT_* or ADD_FAILURE() are used. Therefore a custom logging macro is used.
*/

static bool canQueryLatency(size_t nodeId) {
    hwloc_topology_t topology = nullptr;
    int ret = hwloc_topology_init(&topology);

    if (!GTEST_OUT_EQ(ret, 0)) {
        return false;
    }

    ret = hwloc_topology_load(topology);

    if (!GTEST_OUT_EQ(ret, 0)) {
        return false;
    }

    hwloc_obj_t numaNode =
        hwloc_get_obj_by_type(topology, HWLOC_OBJ_NUMANODE, nodeId);

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
}

INSTANTIATE_TEST_SUITE_P(memspaceLowestLatencyTest, memspaceGetTest,
                         ::testing::Values(memspaceGetParams{
                             canQueryLatency, umfMemspaceLowestLatencyGet}));

INSTANTIATE_TEST_SUITE_P(memspaceLowestLatencyProviderTest,
                         memspaceProviderTest,
                         ::testing::Values(memspaceGetParams{
                             canQueryLatency, umfMemspaceLowestLatencyGet}));
