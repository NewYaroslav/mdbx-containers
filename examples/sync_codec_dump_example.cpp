#define MDBXC_SYNC_ENABLED 1
#include <mdbx_containers/sync.hpp>
#include <cstdint>
#include <cstdio>
#include <iostream>
#include <string>
#include <vector>

namespace {

std::string hex(const std::vector<std::uint8_t>& bytes) {
    static const char* digits = "0123456789abcdef";
    std::string out;
    out.reserve(bytes.size() * 3);
    for (std::size_t i = 0; i < bytes.size(); ++i) {
        if (i > 0 && (i % 16) == 0) out.push_back('\n');
        else if (i > 0) out.push_back(' ');
        out.push_back(digits[(bytes[i] >> 4) & 0xF]);
        out.push_back(digits[bytes[i] & 0xF]);
    }
    return out;
}

} // namespace

int main() {
    using namespace mdbxc::sync;

    ChangeBatch batch;
    batch.version = 1;
    for (int i = 0; i < 16; ++i) {
        batch.origin_node_id[i] = static_cast<std::uint8_t>(0x10 + i);
    }
    batch.seq = 1;
    batch.time_unix_ns = 1700000000000000000ULL;

    ChangeOp op;
    op.op_type = ChangeOpType::Put;
    op.dbi_name = "trades";
    op.storage_key = { 'E','U','R','U','S','D' };
    op.value = { 'B','i','d',':','1','.','1','2','3','4' };
    batch.ops.push_back(op);

    ChangeOp del;
    del.op_type = ChangeOpType::Delete;
    del.op_flags = OP_TOMBSTONE;
    del.dbi_name = "trades";
    del.storage_key = { 'O','L','D' };
    batch.ops.push_back(del);

    const CodecBounds bounds;
    const std::vector<std::uint8_t> bytes = ChangeBatchCodec::encode(batch, &bounds);

    std::cout << "encoded " << bytes.size() << " bytes:\n";
    std::cout << hex(bytes) << "\n";

    const ChangeBatch decoded = ChangeBatchCodec::decode(bytes, nullptr, &bounds);
    std::cout << "\nround-trip ops=" << decoded.ops.size()
              << " seq=" << decoded.seq << "\n";
    return 0;
}