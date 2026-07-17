#include <mdbx_containers/sync.hpp>

#include <cstdint>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

mdbxc::sync::NodeId make_node(std::uint8_t seed) {
    mdbxc::sync::NodeId node{};
    for (int i = 0; i < 16; ++i) {
        node[i] = static_cast<std::uint8_t>(seed + i);
    }
    return node;
}

mdbxc::sync::ChangeBatch make_batch(std::uint8_t seed, std::uint64_t seq) {
    using namespace mdbxc::sync;
    ChangeBatch batch;
    batch.origin_node_id = make_node(seed);
    batch.seq = seq;
    batch.time_unix_ns = 1700000000000000000ULL + seq;

    ChangeOp op;
    op.op_type = ChangeOpType::Put;
    op.dbi_name = "ticks";
    op.storage_key = { static_cast<std::uint8_t>(seq & 0xFFu) };
    op.value = { 0x10, 0x20, static_cast<std::uint8_t>(seed) };
    batch.ops.push_back(op);
    return batch;
}

template<class Fn>
void expect_throw(const std::string& label, Fn&& fn) {
    bool caught = false;
    try {
        fn();
    } catch (...) {
        caught = true;
    }
    if (!caught) {
        throw std::runtime_error(label + ": expected throw");
    }
}

void require_true(bool value, const char* message) {
    if (!value) {
        throw std::runtime_error(message);
    }
}

void test_pull_request_roundtrip() {
    using namespace mdbxc::sync;
    PullRequest request;
    request.requester = make_node(0x10);
    request.db_id = make_node(0x20);
    request.have.last_seq_by_origin[make_node(0xA0)] = 7;
    request.have.last_seq_by_origin[make_node(0xB0)] = 9;
    request.max_batches = 17;
    request.max_bytes = 4096;
    request.request_full_snapshot = true;
    CancellationSource source;
    request.cancel_token = source.token();

    const std::vector<std::uint8_t> bytes =
        TransportMessageCodec::encode_pull_request(request);
    const PullRequest decoded =
        TransportMessageCodec::decode_pull_request(bytes);

    require_true(decoded.requester == request.requester,
                 "PullRequest requester mismatch");
    require_true(decoded.db_id == request.db_id,
                 "PullRequest db_id mismatch");
    require_true(decoded.have.last_seq_for(make_node(0xA0)) == 7,
                 "PullRequest cursor A mismatch");
    require_true(decoded.have.last_seq_for(make_node(0xB0)) == 9,
                 "PullRequest cursor B mismatch");
    require_true(decoded.max_batches == 17,
                 "PullRequest max_batches mismatch");
    require_true(decoded.max_bytes == 4096,
                 "PullRequest max_bytes mismatch");
    require_true(decoded.request_full_snapshot,
                 "PullRequest full snapshot mismatch");
    require_true(!decoded.cancel_token.can_be_cancelled(),
                 "PullRequest cancel token must not be serialized");
}

void test_pull_response_roundtrip() {
    using namespace mdbxc::sync;
    PullResponse response;
    response.remote_have.last_seq_by_origin[make_node(0xA0)] = 4;
    response.remote_have.last_seq_by_origin[make_node(0xB0)] = 8;
    response.batches.push_back(make_batch(0xA0, 5));
    response.batches.push_back(make_batch(0xB0, 9));
    response.has_more = true;
    response.ok = false;
    response.error = "temporary upstream timeout";

    const std::vector<std::uint8_t> bytes =
        TransportMessageCodec::encode_pull_response(response);
    const PullResponse decoded =
        TransportMessageCodec::decode_pull_response(bytes);

    require_true(decoded.remote_have.last_seq_for(make_node(0xA0)) == 4,
                 "PullResponse cursor A mismatch");
    require_true(decoded.remote_have.last_seq_for(make_node(0xB0)) == 8,
                 "PullResponse cursor B mismatch");
    require_true(decoded.batches.size() == 2u,
                 "PullResponse batch count mismatch");
    require_true(decoded.batches[0].origin_node_id == make_node(0xA0),
                 "PullResponse batch A origin mismatch");
    require_true(decoded.batches[1].origin_node_id == make_node(0xB0),
                 "PullResponse batch B origin mismatch");
    require_true(decoded.has_more, "PullResponse has_more mismatch");
    require_true(!decoded.ok, "PullResponse ok mismatch");
    require_true(decoded.error == response.error, "PullResponse error mismatch");
}

void test_push_request_roundtrip() {
    using namespace mdbxc::sync;
    PushRequest request;
    request.sender = make_node(0x30);
    request.db_id = make_node(0x40);
    request.batches.push_back(make_batch(0xC0, 1));
    request.batches.push_back(make_batch(0xC0, 2));
    CancellationSource source;
    request.cancel_token = source.token();

    const std::vector<std::uint8_t> bytes =
        TransportMessageCodec::encode_push_request(request);
    const PushRequest decoded =
        TransportMessageCodec::decode_push_request(bytes);

    require_true(decoded.sender == request.sender,
                 "PushRequest sender mismatch");
    require_true(decoded.db_id == request.db_id,
                 "PushRequest db_id mismatch");
    require_true(decoded.batches.size() == 2u,
                 "PushRequest batch count mismatch");
    require_true(decoded.batches[0].seq == 1,
                 "PushRequest batch 1 seq mismatch");
    require_true(decoded.batches[1].seq == 2,
                 "PushRequest batch 2 seq mismatch");
    require_true(!decoded.cancel_token.can_be_cancelled(),
                 "PushRequest cancel token must not be serialized");
}

void test_push_response_roundtrip() {
    using namespace mdbxc::sync;
    PushResponse response;
    response.receiver_have.last_seq_by_origin[make_node(0xD0)] = 42;
    response.ok = false;
    response.error = "sequence gap";

    const std::vector<std::uint8_t> bytes =
        TransportMessageCodec::encode_push_response(response);
    const PushResponse decoded =
        TransportMessageCodec::decode_push_response(bytes);

    require_true(decoded.receiver_have.last_seq_for(make_node(0xD0)) == 42,
                 "PushResponse cursor mismatch");
    require_true(!decoded.ok, "PushResponse ok mismatch");
    require_true(decoded.error == response.error, "PushResponse error mismatch");
}

void test_message_header_rejections() {
    using namespace mdbxc::sync;
    const std::vector<std::uint8_t> bytes =
        TransportMessageCodec::encode_pull_request(PullRequest());

    expect_throw("empty input", [] {
        (void)TransportMessageCodec::decode_pull_request(
            std::vector<std::uint8_t>());
    });

    expect_throw("magic mismatch", [bytes] {
        std::vector<std::uint8_t> bad = bytes;
        bad[0] = 0xFF;
        (void)TransportMessageCodec::decode_pull_request(bad);
    });

    expect_throw("version mismatch", [bytes] {
        std::vector<std::uint8_t> bad = bytes;
        bad[8] = 0xFF;
        bad[9] = 0xFF;
        (void)TransportMessageCodec::decode_pull_request(bad);
    });

    expect_throw("unknown flags", [bytes] {
        std::vector<std::uint8_t> bad = bytes;
        bad[11] = 0x01;
        (void)TransportMessageCodec::decode_pull_request(bad);
    });

    expect_throw("wrong message type", [] {
        const std::vector<std::uint8_t> bad =
            TransportMessageCodec::encode_pull_response(PullResponse());
        (void)TransportMessageCodec::decode_pull_request(bad);
    });

    expect_throw("trailing bytes", [bytes] {
        std::vector<std::uint8_t> bad = bytes;
        bad.push_back(0xEE);
        (void)TransportMessageCodec::decode_pull_request(bad);
    });

    expect_throw("invalid bool", [bytes] {
        std::vector<std::uint8_t> bad = bytes;
        bad[bad.size() - 1u] = 2u;
        (void)TransportMessageCodec::decode_pull_request(bad);
    });
}

void test_bounds_rejections() {
    using namespace mdbxc::sync;

    expect_throw("cursor origins bound", [] {
        CodecBounds bounds;
        bounds.max_cursor_origins = 1;
        PullRequest request;
        request.have.last_seq_by_origin[make_node(0xA0)] = 1;
        request.have.last_seq_by_origin[make_node(0xB0)] = 2;
        (void)TransportMessageCodec::encode_pull_request(request, &bounds);
    });

    expect_throw("batches per message bound", [] {
        CodecBounds bounds;
        bounds.max_batches_per_message = 1;
        PullResponse response;
        response.batches.push_back(make_batch(0xA0, 1));
        response.batches.push_back(make_batch(0xA0, 2));
        (void)TransportMessageCodec::encode_pull_response(response, &bounds);
    });

    expect_throw("error string bound", [] {
        CodecBounds bounds;
        bounds.max_error_len = 4;
        PushResponse response;
        response.ok = false;
        response.error = "too long";
        (void)TransportMessageCodec::encode_push_response(response, &bounds);
    });

    expect_throw("transport message bytes bound", [] {
        CodecBounds bounds;
        bounds.max_transport_message_bytes = 16;
        PullRequest request;
        (void)TransportMessageCodec::encode_pull_request(request, &bounds);
    });
}

void test_golden_header_shape() {
    using namespace mdbxc::sync;
    const std::vector<std::uint8_t> bytes =
        TransportMessageCodec::encode_pull_request(PullRequest());
    static const std::uint8_t expected_magic[8] =
        { 'M','D','B','X','C','P','R','T' };
    for (int i = 0; i < 8; ++i) {
        require_true(bytes[i] == expected_magic[i],
                     "TransportMessageCodec magic mismatch");
    }
    require_true(bytes[8] == 1u && bytes[9] == 0u,
                 "TransportMessageCodec version mismatch");
    require_true(bytes[10] == 1u,
                 "TransportMessageCodec pull request type mismatch");
    require_true(bytes[11] == 0u && bytes[12] == 0u &&
                 bytes[13] == 0u && bytes[14] == 0u,
                 "TransportMessageCodec flags mismatch");
}

} // namespace

int main() {
    test_pull_request_roundtrip();
    test_pull_response_roundtrip();
    test_push_request_roundtrip();
    test_push_response_roundtrip();
    test_message_header_rejections();
    test_bounds_rejections();
    test_golden_header_shape();
    return 0;
}
