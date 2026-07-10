#include <mdbx_containers/sync.hpp>
#include <cstdint>
#include <cstdio>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

void assert_eq(const std::string& label, std::size_t a, std::size_t b) {
    if (a != b) {
        throw std::runtime_error(label + ": expected " + std::to_string(b) +
                                 " got " + std::to_string(a));
    }
}

void assert_eq_bytes(const std::string& label,
                     const std::vector<std::uint8_t>& a,
                     const std::vector<std::uint8_t>& b) {
    if (a.size() != b.size()) {
        throw std::runtime_error(label + ": size mismatch " +
                                 std::to_string(a.size()) + " vs " +
                                 std::to_string(b.size()));
    }
    for (std::size_t i = 0; i < a.size(); ++i) {
        if (a[i] != b[i]) {
            throw std::runtime_error(label + ": byte mismatch at " +
                                     std::to_string(i));
        }
    }
}

mdbxc::sync::ChangeBatch make_batch() {
    using namespace mdbxc::sync;
    ChangeBatch batch;
    batch.version = 1;
    batch.batch_flags = BATCH_NONE;
    for (int i = 0; i < 16; ++i) {
        batch.origin_node_id[i] = static_cast<std::uint8_t>(i + 1);
    }
    batch.seq = 42;
    batch.time_unix_ns = 1700000000000000000ULL;

    ChangeOp op;
    op.op_type = ChangeOpType::Put;
    op.op_flags = OP_HAS_REVISION_KEY;
    op.dbi_flags = 0;
    op.dbi_name = "trades";
    op.storage_key = { 0x01, 0x02, 0x03, 0x04 };
    op.value.assign(64, 0xAA);
    op.revision_key = { 0x10, 0x20 };

    ChangeOp del;
    del.op_type = ChangeOpType::Delete;
    del.op_flags = OP_TOMBSTONE;
    del.dbi_name = "trades";
    del.storage_key = { 0x05, 0x06 };

    batch.ops.push_back(op);
    batch.ops.push_back(del);
    return batch;
}

void test_roundtrip_full() {
    using namespace mdbxc;
    using namespace mdbxc::sync;
    const CodecBounds bounds;
    ChangeBatch batch = make_batch();
    std::vector<std::uint8_t> bytes = ChangeBatchCodec::encode(batch, &bounds);
    ChangeBatch decoded = ChangeBatchCodec::decode(bytes, nullptr, &bounds);

    assert_eq("version", decoded.version, batch.version);
    assert_eq("ops count", decoded.ops.size(), batch.ops.size());
    assert_eq("seq", decoded.seq, batch.seq);
    assert_eq("time", decoded.time_unix_ns, batch.time_unix_ns);
    for (int i = 0; i < 16; ++i) {
        assert_eq("origin byte " + std::to_string(i),
                  decoded.origin_node_id[i], batch.origin_node_id[i]);
    }
    const ChangeOp& od = decoded.ops[0];
    const ChangeOp& os = batch.ops[0];
    assert_eq("dbi_name 0", od.dbi_name.size(), os.dbi_name.size());
    assert_eq("storage_key 0", od.storage_key.size(), os.storage_key.size());
    assert_eq("value 0", od.value.size(), os.value.size());
    assert_eq("revision_key 0", od.revision_key.size(), os.revision_key.size());
    assert_eq("op_flags 0", od.op_flags, static_cast<std::uint32_t>(os.op_flags));

    const ChangeOp& dd = decoded.ops[1];
    const ChangeOp& ds = batch.ops[1];
    assert_eq("op_type 1", static_cast<std::uint8_t>(dd.op_type),
              static_cast<std::uint8_t>(ds.op_type));
    assert_eq("tombstone flags", dd.op_flags,
              static_cast<std::uint32_t>(OP_TOMBSTONE));
    assert_eq("value empty", dd.value.size(), 0u);
}

void test_roundtrip_stream_mode() {
    using namespace mdbxc::sync;
    const CodecBounds bounds;
    const ChangeBatch batch = make_batch();
    std::vector<std::uint8_t> bytes = ChangeBatchCodec::encode(batch, &bounds);
    const std::size_t expected_consumed = bytes.size();
    bytes.push_back(0xFF);
    bytes.push_back(0xEE);
    bytes.push_back(0xDD);

    std::size_t consumed = 0;
    const ChangeBatch decoded = ChangeBatchCodec::decode(bytes, &consumed, &bounds);
    assert_eq("stream consumed", consumed, expected_consumed);
    assert_eq("stream ops", decoded.ops.size(), batch.ops.size());
    assert_eq("stream seq", decoded.seq, batch.seq);
    assert_eq("stream version", decoded.version, batch.version);
}

void test_roundtrip_idempotent() {
    using namespace mdbxc::sync;
    const CodecBounds bounds;
    const ChangeBatch batch = make_batch();
    const std::vector<std::uint8_t> bytes1 = ChangeBatchCodec::encode(batch, &bounds);
    const std::vector<std::uint8_t> bytes2 = ChangeBatchCodec::encode(batch, &bounds);
    assert_eq_bytes("deterministic encode", bytes1, bytes2);
}

void test_roundtrip_no_ops() {
    using namespace mdbxc::sync;
    const CodecBounds bounds;
    ChangeBatch batch;
    batch.seq = 7;
    batch.origin_node_id[0] = 0xAB;
    const std::vector<std::uint8_t> bytes = ChangeBatchCodec::encode(batch, &bounds);
    const ChangeBatch decoded = ChangeBatchCodec::decode(bytes, nullptr, &bounds);
    assert_eq("empty ops", decoded.ops.size(), 0u);
    assert_eq("seq 7", decoded.seq, 7u);
}

} // namespace

int main() {
    test_roundtrip_full();
    test_roundtrip_idempotent();
    test_roundtrip_no_ops();
    test_roundtrip_stream_mode();
    return 0;
}