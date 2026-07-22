#include <mdbx_containers.hpp>

int main() {
    mdbxc::Config config;
    config.max_dbs = 4;
    return config.max_dbs == 4 ? 0 : 1;
}
