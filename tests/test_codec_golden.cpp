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

    static const std::uint8_t expected_magic[8] = { 'M','D','B','X','C','S','Y','N' };
    for (int i = 0; i < 8; ++i) {
        if (bytes[i] != expected_magic[i]) {
            throw std::runtime_error("magic byte mismatch");
        }
    }
    expect("codec_version low", bytes[8], 1u);
    expect("codec_version high", bytes[9], 0u);
    expect("batch_version b0", bytes[10], 1u);
    expect("batch_version b1", bytes[11], 0u);
    expect("batch_version b2", bytes[12], 0u);
    expect("batch_version b3", bytes[13], 0u);
    expect("batch_flags b0", bytes[14], 0u);
    expect("batch_flags b1", bytes[15], 0u);
    expect("batch_flags b2", bytes[16], 0u);
    expect("batch_flags b3", bytes[17], 0u);
    expect("origin byte 0", bytes[18], 0x10u);
    expect("origin byte 15", bytes[33], 0x1Fu);
    expect("seq b0", bytes[34], 0x08u);
    expect("seq b7", bytes[41], 0x01u);
    expect("time b0", bytes[42], 0x88u);
    expect("time b7", bytes[49], 0x11u);
    expect("ops_count b0", bytes[50], 1u);
    expect("ops_count b3", bytes[53], 0u);
}

void test_golden_trailing_bytes_rejected() {
    using namespace mdbxc::sync;
    const CodecBounds bounds;
    ChangeBatch batch;
    ChangeOp op;
    op.op_type = ChangeOpType::Put;
    op.dbi_name = "t";
    op.storage_key = { 0x01 };
    op.value = { 0x02 };
    batch.ops.push_back(op);
    std::vector<std::uint8_t> bytes = ChangeBatchCodec::encode(batch, &bounds);
    bytes.push_back(0xFF);
    bool caught = false;
    try {
        (void)ChangeBatchCodec::decode(bytes, nullptr, &bounds);
    } catch (const std::runtime_error&) {
        caught = true;
    }
    if (!caught) {
        throw std::runtime_error("decode without bytes_read must reject trailing bytes");
    }
}

void test_decode_exact_rejects_trailing() {
    using namespace mdbxc::sync;
    const CodecBounds bounds;
    ChangeBatch batch;
    ChangeOp op;
    op.op_type = ChangeOpType::Put;
    op.dbi_name = "t";
    op.storage_key = { 0x01 };
    op.value = { 0x02 };
    batch.ops.push_back(op);
    std::vector<std::uint8_t> bytes = ChangeBatchCodec::encode(batch, &bounds);
    bytes.push_back(0xFF);
    bool caught = false;
    try {
        (void)ChangeBatchCodec::decode_exact(bytes, &bounds);
    } catch (const std::runtime_error&) {
        caught = true;
    }
    if (!caught) {
        throw std::runtime_error("decode_exact must reject trailing bytes");
    }
}

void test_encoder_rejects_bad_batch_version() {
    using namespace mdbxc::sync;
    const CodecBounds bounds;
    ChangeBatch batch;
    batch.version = 999;
    bool caught = false;
    try {
        (void)ChangeBatchCodec::encode(batch, &bounds);
    } catch (const std::logic_error&) {
        caught = true;
    }
    if (!caught) {
        throw std::runtime_error("encoder must reject batch.version != 1");
    }
}

void test_encoder_rejects_unknown_batch_flags() {
    using namespace mdbxc::sync;
    const CodecBounds bounds;
    ChangeBatch batch;
    batch.batch_flags = 0x80000000u;
    bool caught = false;
    try {
        (void)ChangeBatchCodec::encode(batch, &bounds);
    } catch (const std::logic_error&) {
        caught = true;
    }
    if (!caught) {
        throw std::runtime_error("encoder must reject unknown batch_flags");
    }
}

void test_encoder_rejects_unknown_op_type() {
    using namespace mdbxc::sync;
    const CodecBounds bounds;
    ChangeBatch batch;
    ChangeOp op;
    op.op_type = static_cast<ChangeOpType>(99);
    op.dbi_name = "t";
    op.storage_key = { 0x01 };
    op.value = { 0x02 };
    batch.ops.push_back(op);
    bool caught = false;
    try {
        (void)ChangeBatchCodec::encode(batch, &bounds);
    } catch (const std::logic_error&) {
        caught = true;
    }
    if (!caught) {
        throw std::runtime_error("encoder must reject unknown ChangeOpType");
    }
}

void test_batch_has_more_roundtrip() {
    using namespace mdbxc::sync;
    const CodecBounds bounds;
    ChangeBatch batch;
    batch.batch_flags = BATCH_HAS_MORE;
    ChangeOp op;
    op.op_type = ChangeOpType::Put;
    op.dbi_name = "t";
    op.storage_key = { 0x01 };
    op.value = { 0x02 };
    batch.ops.push_back(op);
    const std::vector<std::uint8_t> bytes = ChangeBatchCodec::encode(batch, &bounds);
    const ChangeBatch decoded = ChangeBatchCodec::decode_exact(bytes, &bounds);
    if ((decoded.batch_flags & BATCH_HAS_MORE) == 0) {
        throw std::runtime_error("BATCH_HAS_MORE must survive roundtrip");
    }
}

} // namespace

int main() {
    test_golden_magic_and_version();
    test_golden_trailing_bytes_rejected();
    test_decode_exact_rejects_trailing();
    test_encoder_rejects_bad_batch_version();
    test_encoder_rejects_unknown_batch_flags();
    test_encoder_rejects_unknown_op_type();
    test_batch_has_more_roundtrip();
    return 0;
}