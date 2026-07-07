#include <mdbx_containers/Sync.hpp>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

std::vector<std::uint8_t> make_fixed_batch() {
    using namespace mdbxc::sync;
    ChangeBatch batch;
    batch.version = 1;
    batch.batch_flags = BATCH_NONE;
    for (int i = 0; i < 16; ++i) {
        batch.origin_node_id[i] = static_cast<std::uint8_t>(0x10 + i);
    }
    batch.seq = 0x0102030405060708ULL;
    batch.time_unix_ns = 0x1122334455667788ULL;

    ChangeOp op;
    op.op_type = ChangeOpType::Put;
    op.op_flags = OP_NONE;
    op.dbi_flags = 0;
    op.dbi_name = "t";
    op.storage_key = { 0xAA, 0xBB };
    op.value = { 0xCC, 0xDD, 0xEE };
    batch.ops.push_back(op);

    const CodecBounds bounds;
    return ChangeBatchCodec::encode(batch, &bounds);
}

void expect(const std::string& label, std::size_t actual, std::size_t expected) {
    if (actual != expected) {
        throw std::runtime_error(label + ": expected " + std::to_string(expected) +
                                 " got " + std::to_string(actual));
    }
}

void test_golden_magic_and_version() {
    using namespace mdbxc::sync;
    const std::vector<std::uint8_t> bytes = make_fixed_batch();

    static const std::uint8_t expected_magic[7] = { 'M','D','B','X','C','S','Y' };
    for (int i = 0; i < 7; ++i) {
        if (bytes[i] != expected_magic[i]) {
            throw std::runtime_error("magic byte mismatch");
        }
    }
    expect("codec_version low", bytes[7], 1u);
    expect("codec_version high", bytes[8], 0u);
    expect("batch_version b0", bytes[9], 1u);
    expect("batch_version b1", bytes[10], 0u);
    expect("batch_version b2", bytes[11], 0u);
    expect("batch_version b3", bytes[12], 0u);
    expect("batch_flags b0", bytes[13], 0u);
    expect("batch_flags b1", bytes[14], 0u);
    expect("batch_flags b2", bytes[15], 0u);
    expect("batch_flags b3", bytes[16], 0u);
    expect("origin byte 0", bytes[17], 0x10u);
    expect("origin byte 15", bytes[32], 0x1Fu);
    expect("seq b0", bytes[33], 0x08u);
    expect("seq b7", bytes[40], 0x01u);
    expect("time b0", bytes[41], 0x88u);
    expect("time b7", bytes[48], 0x11u);
    expect("ops_count b0", bytes[49], 1u);
    expect("ops_count b3", bytes[52], 0u);
}

} // namespace

int main() {
    test_golden_magic_and_version();
    return 0;
}