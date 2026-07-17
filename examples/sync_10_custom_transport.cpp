/**
 * \ingroup mdbxc_examples
 * \brief Minimal custom ISyncPeer over byte buffers.
 *
 * This is the smallest useful shape for an application-specific transport:
 * implement `ISyncPeer`, encode request DTOs with `TransportMessageCodec`,
 * send the bytes through the application channel, then decode response DTOs.
 *
 * The example keeps the "remote channel" in-process so it can run everywhere.
 * Replace `send_pull_bytes()` / `send_push_bytes()` with a socket, IPC, file,
 * message queue, or another application transport.
 *
 * Expected output:
 *   [custom transport pull] applied 2 batch(es)
 *   [custom transport push] applied 1 batch(es)
 *   OK: sync_10_custom_transport
 */

#include "sync_example_utils.hpp"

#include <cstdio>
#include <cstdint>
#include <exception>
#include <memory>
#include <string>
#include <vector>

namespace {

class EncodedLoopbackPeer : public mdbxc::sync::ISyncPeer {
public:
    explicit EncodedLoopbackPeer(mdbxc::sync::SyncEngine& remote)
        : m_remote(remote),
          m_cancel_count(0),
          m_last_pull_token_cancellable(false) {}

    mdbxc::sync::PullResponse pull(
            const mdbxc::sync::PullRequest& request) override {
        m_last_pull_token_cancellable =
            request.cancel_token.can_be_cancelled();

        const std::vector<std::uint8_t> request_bytes =
            mdbxc::sync::TransportMessageCodec::encode_pull_request(
                request);
        const std::vector<std::uint8_t> response_bytes =
            send_pull_bytes(request_bytes);
        return mdbxc::sync::TransportMessageCodec::decode_pull_response(
            response_bytes);
    }

    mdbxc::sync::PushResponse push(
            const mdbxc::sync::PushRequest& request) override {
        const std::vector<std::uint8_t> request_bytes =
            mdbxc::sync::TransportMessageCodec::encode_push_request(
                request);
        const std::vector<std::uint8_t> response_bytes =
            send_push_bytes(request_bytes);
        return mdbxc::sync::TransportMessageCodec::decode_push_response(
            response_bytes);
    }

    void request_cancel() override {
        // A real transport would interrupt the in-flight channel operation.
        ++m_cancel_count;
    }

    bool last_pull_token_cancellable() const {
        return m_last_pull_token_cancellable;
    }

    std::size_t cancel_count() const { return m_cancel_count; }

private:
    std::vector<std::uint8_t> send_pull_bytes(
            const std::vector<std::uint8_t>& request_bytes) {
        // Remote side: decode bytes, dispatch to SyncEngine, encode response.
        const mdbxc::sync::PullRequest request =
            mdbxc::sync::TransportMessageCodec::decode_pull_request(
                request_bytes);
        const mdbxc::sync::PullResponse response =
            m_remote.handle_pull(request);
        return mdbxc::sync::TransportMessageCodec::encode_pull_response(
            response);
    }

    std::vector<std::uint8_t> send_push_bytes(
            const std::vector<std::uint8_t>& request_bytes) {
        // Remote side: decode bytes, dispatch to SyncEngine, encode response.
        const mdbxc::sync::PushRequest request =
            mdbxc::sync::TransportMessageCodec::decode_push_request(
                request_bytes);
        const mdbxc::sync::PushResponse response =
            m_remote.handle_push(request);
        return mdbxc::sync::TransportMessageCodec::encode_push_response(
            response);
    }

    mdbxc::sync::SyncEngine& m_remote;
    std::size_t m_cancel_count;
    bool m_last_pull_token_cancellable;
};

} // namespace

int main() {
    const std::string primary_path = "sync_10_primary.mdbx";
    const std::string replica_path = "sync_10_replica.mdbx";
    sync_example::cleanup(primary_path);
    sync_example::cleanup(replica_path);

    const mdbxc::sync::NodeId primary_node =
        sync_example::make_node(0xF0);
    const mdbxc::sync::NodeId replica_node =
        sync_example::make_node(0xF1);
    const mdbxc::sync::DbId db_id =
        sync_example::make_node(0xF2);

    try {
        std::shared_ptr<mdbxc::Connection> primary =
            sync_example::open(primary_path);
        std::shared_ptr<mdbxc::Connection> replica =
            sync_example::open(replica_path);

        mdbxc::sync::SyncEngine primary_engine(primary);
        mdbxc::sync::SyncEngine replica_engine(replica);
        primary_engine.initialize_local_identity(primary_node, db_id);
        replica_engine.initialize_local_identity(replica_node, db_id);

        mdbxc::sync::ThreadLocalChangeAccumulator capture(primary);
        mdbxc::KeyValueTable<int, std::string> primary_ticks(primary, "ticks");

        primary->attach_sync_capture(&capture);
        primary_ticks.insert_or_assign(1, "BTC/USD");
        primary_ticks.insert_or_assign(2, "ETH/USD");
        primary->detach_sync_capture();

        EncodedLoopbackPeer primary_peer(primary_engine);

        mdbxc::sync::CancellationSource pull_cancel;
        mdbxc::sync::PullRequest pull;
        pull.requester = replica_node;
        pull.db_id = db_id;
        pull.have = replica_engine.applied_cursor();
        pull.max_batches = 100;
        pull.cancel_token = pull_cancel.token();

        const mdbxc::sync::PullResponse pulled = primary_peer.pull(pull);
        sync_example::require(pulled.ok,
                              "custom pull failed: " + pulled.error);
        sync_example::require(primary_peer.last_pull_token_cancellable(),
                              "custom peer did not receive cancel token");

        mdbxc::sync::PushRequest local_apply;
        local_apply.sender = primary_node;
        local_apply.db_id = db_id;
        local_apply.batches = pulled.batches;
        const mdbxc::sync::PushResponse applied =
            replica_engine.handle_push(local_apply);
        sync_example::require(applied.ok,
                              "local apply failed: " + applied.error);
        std::printf("[custom transport pull] applied %zu batch(es)\n",
                    pulled.batches.size());

        primary->attach_sync_capture(&capture);
        primary_ticks.insert_or_assign(3, "SOL/USD");
        primary->detach_sync_capture();

        EncodedLoopbackPeer replica_peer(replica_engine);
        const mdbxc::sync::PushRequest push =
            primary_engine.make_push_request(3, 0);
        const mdbxc::sync::PushResponse pushed = replica_peer.push(push);
        sync_example::require(pushed.ok,
                              "custom push failed: " + pushed.error);
        std::printf("[custom transport push] applied %zu batch(es)\n",
                    push.batches.size());

        replica_peer.request_cancel();
        sync_example::require(replica_peer.cancel_count() == 1u,
                              "custom peer did not forward cancellation");

        mdbxc::KeyValueTable<int, std::string> replica_ticks(replica, "ticks");
        sync_example::require(
            sync_example::kv_or_throw(replica, replica_ticks, 1,
                                      "ticks[1]") == "BTC/USD",
            "replica ticks[1] mismatch");
        sync_example::require(
            sync_example::kv_or_throw(replica, replica_ticks, 2,
                                      "ticks[2]") == "ETH/USD",
            "replica ticks[2] mismatch");
        sync_example::require(
            sync_example::kv_or_throw(replica, replica_ticks, 3,
                                      "ticks[3]") == "SOL/USD",
            "replica ticks[3] mismatch");

        sync_example::disconnect_and_cleanup(primary, primary_path);
        sync_example::disconnect_and_cleanup(replica, replica_path);
        std::printf("OK: sync_10_custom_transport\n");
        return 0;
    } catch (const std::exception& e) {
        std::printf("FAIL: %s\n", e.what());
        sync_example::cleanup(primary_path);
        sync_example::cleanup(replica_path);
        return 1;
    }
}
