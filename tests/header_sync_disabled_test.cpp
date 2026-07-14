#include <mdbx_containers/sync.hpp>

#ifndef MDBXC_SYNC_ENABLED
#error "sync.hpp must define MDBXC_SYNC_ENABLED when it is omitted by callers"
#endif

#if MDBXC_SYNC_ENABLED
#error "header_sync_disabled_test must be compiled without enabling sync"
#endif

#include <mdbx_containers.hpp>

#include "test_assert.hpp"

int main() {
    MDBXC_TEST_ASSERT(MDBXC_SYNC_ENABLED == 0);

    mdbxc::Config config;
    config.max_dbs = 4;
    MDBXC_TEST_ASSERT(config.max_dbs == 4);

    const mdbxc::VectorMetric metric = mdbxc::VectorMetric::COSINE;
    MDBXC_TEST_ASSERT(metric == mdbxc::VectorMetric::COSINE);

    return 0;
}
