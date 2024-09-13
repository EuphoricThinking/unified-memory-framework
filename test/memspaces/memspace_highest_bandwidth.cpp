// Copyright (C) 2024 Intel Corporation
// Under the Apache License v2.0 with LLVM Exceptions. See LICENSE.TXT.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include <umf/memspace.h>

#include "memspace_fixtures.hpp"
#include "memspace_helpers.hpp"
#include "memspace_internal.h"
#include "test_helpers.h"

static void canQueryBandwidth(size_t nodeId) {
    hwloc_topology_t topology = nullptr;
    int ret = hwloc_topology_init(&topology);

    if (!GTEST_OUT_EQ(ret, 0)){ // EQ
        // return FATAL_RES;
        GTEST_FAIL() << "SISI";
    }

    ret = hwloc_topology_load(topology);

    if (!GTEST_OUT_EQ(ret, 0)) {
        //return FATAL_RES;
        GTEST_FAIL() << "lalal";
    }

    hwloc_obj_t numaNode =
        hwloc_get_obj_by_type(topology, HWLOC_OBJ_NUMANODE, nodeId);

    if (!GTEST_OUT_NE(numaNode, nullptr)){
        // return FATAL_RES;
        GTEST_FAIL() << "dada";
    }

    // Setup initiator structure.
    struct hwloc_location initiator;
    initiator.location.cpuset = numaNode->cpuset;
    initiator.type = hwloc_location_type_alias::HWLOC_LOCATION_TYPE_CPUSET;

    hwloc_uint64_t value = 0;
    ret = hwloc_memattr_get_value(topology, HWLOC_MEMATTR_ID_BANDWIDTH,
                                  numaNode, &initiator, 0, &value);

    hwloc_topology_destroy(topology);

    if (!GTEST_OUT_EQ(ret, 0)) { // EQ
        // return SKIP_RES;
        GTEST_SKIP() << "fifif";
    }
    else {
        return;
    }
}

INSTANTIATE_TEST_SUITE_P(memspaceLowestLatencyTest, memspaceGetTest,
                         ::testing::Values(memspaceGetParams{
                             canQueryBandwidth,
                             umfMemspaceHighestBandwidthGet}));

INSTANTIATE_TEST_SUITE_P(memspaceLowestLatencyProviderTest,
                         memspaceProviderTest,
                         ::testing::Values(memspaceGetParams{
                             canQueryBandwidth,
                             umfMemspaceHighestBandwidthGet}));
