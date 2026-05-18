/**
 * \ingroup mdbxc_examples
 * Demonstrates a singleton value table for application state.
 */

#include <mdbx_containers/ValueTable.hpp>

#include <cstring>
#include <iostream>
#include <vector>

struct AppState {
    int schema_version;
    int active_profiles;

    std::vector<uint8_t> to_bytes() const {
        std::vector<uint8_t> bytes(sizeof(schema_version) + sizeof(active_profiles));
        std::memcpy(bytes.data(), &schema_version, sizeof(schema_version));
        std::memcpy(bytes.data() + sizeof(schema_version),
                    &active_profiles,
                    sizeof(active_profiles));
        return bytes;
    }

    static AppState from_bytes(const void* data, size_t size) {
        if (size != sizeof(int) + sizeof(int)) {
            throw std::runtime_error("Invalid data size for AppState");
        }
        AppState out{};
        const uint8_t* ptr = static_cast<const uint8_t*>(data);
        std::memcpy(&out.schema_version, ptr, sizeof(out.schema_version));
        std::memcpy(&out.active_profiles, ptr + sizeof(out.schema_version), sizeof(out.active_profiles));
        return out;
    }
};

int main() {
    mdbxc::Config config;
    config.pathname = "value_table_example_db";
    config.max_dbs = 1;

    auto conn = mdbxc::Connection::create(config);
    mdbxc::ValueTable<AppState> state(conn, "app_state");

    if (!state.has_value()) {
        AppState initial{};
        initial.schema_version = 1;
        initial.active_profiles = 0;
        state.set(initial);
    }

    state.update([](AppState& value) {
        value.active_profiles += 1;
    });

    AppState loaded = state.get();
    std::cout << "schema version: " << loaded.schema_version << '\n';
    std::cout << "active profiles: " << loaded.active_profiles << '\n';

    return 0;
}
