#include <mdbx_containers/sync.hpp>
#include <stdexcept>
#include <string>

namespace {

template<class Fn>
void expect_throw(const std::string& label, Fn&& fn) {
    bool caught = false;
    try { fn(); } catch (...) { caught = true; }
    if (!caught) {
        throw std::runtime_error(label + ": expected throw, nothing thrown");
    }
}

void test_unknown_batch_flag_rejected() {
    expect_throw("unknown batch flag", [] {
        using namespace mdbxc::sync;
        const CodecBounds b;
        ChangeBatch batch;
        batch.batch_flags = 0x80000000u;
        batch.origin_node_id[0] = 1;
        const std::vector<std::uint8_t> bytes = ChangeBatchCodec::encode(batch, &b);
        (void)ChangeBatchCodec::decode(bytes, nullptr, &b);
    });
}

void test_zstd_flag_rejected_on_decode() {
    expect_throw("zstd unsupported", [] {
        using namespace mdbxc::sync;
        const CodecBounds b;
        ChangeBatch batch;
        batch.batch_flags = BATCH_COMPRESSED_ZSTD;
        batch.origin_node_id[0] = 1;
        (void)ChangeBatchCodec::encode(batch, &b);
    });
}

void test_unknown_op_flag_rejected() {
    expect_throw("unknown op flag", [] {
        using namespace mdbxc::sync;
        const CodecBounds b;
        ChangeBatch batch;
        batch.origin_node_id[0] = 1;
        ChangeOp op;
        op.op_type = ChangeOpType::Put;
        op.op_flags = 0x80000000u;
        op.dbi_name = "t";
        op.storage_key = { 0x01 };
        op.value = { 0x02 };
        batch.ops.push_back(op);
        const std::vector<std::uint8_t> bytes = ChangeBatchCodec::encode(batch, &b);
        (void)ChangeBatchCodec::decode(bytes, nullptr, &b);
    });
}

void test_version_mismatch_rejected() {
    expect_throw("codec version mismatch", [] {
        using namespace mdbxc::sync;
        const CodecBounds b;
        std::vector<std::uint8_t> bytes = ChangeBatchCodec::encode(ChangeBatch{}, &b);
        // Patch the codec_version field to an unsupported value.
        bytes[7] = 0xFF;
        bytes[8] = 0xFF;
        (void)ChangeBatchCodec::decode(bytes, nullptr, &b);
    });
}

void test_magic_mismatch_rejected() {
    expect_throw("magic mismatch", [] {
        using namespace mdbxc::sync;
        const CodecBounds b;
        std::vector<std::uint8_t> bytes = ChangeBatchCodec::encode(ChangeBatch{}, &b);
        bytes[0] = 'X';
        (void)ChangeBatchCodec::decode(bytes, nullptr, &b);
    });
}

} // namespace

int main() {
    test_unknown_batch_flag_rejected();
    test_zstd_flag_rejected_on_decode();
    test_unknown_op_flag_rejected();
    test_version_mismatch_rejected();
    test_magic_mismatch_rejected();
    return 0;
}