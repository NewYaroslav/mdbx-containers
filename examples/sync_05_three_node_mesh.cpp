/**
 * \ingroup mdbxc_examples
 * \brief Three independent databases exchange local changelogs pairwise.
 *
 * Applied remote batches are not rewritten into the receiver's local
 * changelog:
 *
 *   A local changelog contains only A's writes.
 *   B local changelog contains only B's writes.
 *   C local changelog contains only C's writes.
 *
 * Applying A's batch to B updates B's user tables, but does not append A's
 * batch to B's changelog. Therefore C cannot obtain A's batch by pulling from
 * B. Without a hub/relay that explicitly retains remote-origin batches, this
 * example must do a full pairwise exchange:
 *
 *        A
 *       / \
 *      v   v
 *      B<->C
 *
 * Required directed pulls: A->B, A->C, B->A, B->C, C->A, C->B.
 *
 * Expected output:
 *   [mesh A] committed local write
 *   [mesh B] committed local write
 *   [mesh C] committed local write
 *   [mesh A -> B] applied 1 batch(es)
 *   [mesh A -> C] applied 1 batch(es)
 *   [mesh B -> A] applied 1 batch(es)
 *   [mesh B -> C] applied 1 batch(es)
 *   [mesh C -> A] applied 1 batch(es)
 *   [mesh C -> B] applied 1 batch(es)
 *   OK: sync_05_three_node_mesh
 */

#include "sync_example_utils.hpp"

#include <cstdio>
#include <cstdint>
#include <exception>
#include <memory>
#include <string>

namespace {

struct NodeDb {
    std::string name;
    std::string path;
    mdbxc::sync::NodeId node_id;
    std::shared_ptr<mdbxc::Connection> conn;
    std::unique_ptr<mdbxc::sync::SyncEngine> engine;
};

void open_node(NodeDb& node, const mdbxc::sync::DbId& db_id) {
    node.conn = sync_example::open(node.path);
    node.engine.reset(new mdbxc::sync::SyncEngine(node.conn));
    node.engine->initialize_local_identity(node.node_id, db_id);
}

void write_local(NodeDb& node, int key, const std::string& value) {
    mdbxc::sync::ThreadLocalChangeAccumulator capture(node.conn);
    node.conn->attach_sync_capture(&capture);
    mdbxc::KeyValueTable<int, std::string> shared(node.conn, "shared");
    shared.insert_or_assign(key, value);
    node.conn->detach_sync_capture();
    std::printf("[mesh %s] committed local write\n", node.name.c_str());
}

void sync_pair(NodeDb& source, NodeDb& target,
               const mdbxc::sync::DbId& db_id) {
    mdbxc::sync::DirectSyncPeer peer(source.engine.get());
    mdbxc::sync::PullRequest pull;
    pull.requester = target.node_id;
    pull.db_id = db_id;
    pull.have = target.engine->applied_cursor();
    pull.max_batches = 1;

    std::size_t applied = 0;
    bool has_more = false;
    do {
        const mdbxc::sync::SyncCursor before = pull.have;
        const mdbxc::sync::PullResponse pulled = peer.pull(pull);
        sync_example::require(pulled.ok,
                              source.name + " -> " + target.name +
                                  " mesh pull failed: " + pulled.error);
        sync_example::require(!pulled.has_more || !pulled.batches.empty(),
                              source.name + " -> " + target.name +
                                  " mesh has_more without batches");

        if (!pulled.batches.empty()) {
            mdbxc::sync::PushRequest push;
            push.sender = source.node_id;
            push.db_id = db_id;
            push.batches = pulled.batches;

            const mdbxc::sync::PushResponse pushed =
                target.engine->handle_push(push);
            sync_example::require(pushed.ok,
                                  source.name + " -> " + target.name +
                                      " mesh apply failed: " + pushed.error);
            applied += pulled.batches.size();
            pull.have = pushed.receiver_have;
        } else {
            pull.have = target.engine->applied_cursor();
        }

        has_more = pulled.has_more;
        sync_example::require(!has_more ||
                                  pull.have.last_seq_by_origin !=
                                      before.last_seq_by_origin,
                              source.name + " -> " + target.name +
                                  " mesh pagination made no cursor progress");
    } while (has_more);

    std::printf("[mesh %s -> %s] applied %zu batch(es)\n",
                source.name.c_str(), target.name.c_str(), applied);
}

void require_shared(NodeDb& node, int key, const std::string& value) {
    mdbxc::KeyValueTable<int, std::string> shared(node.conn, "shared");
    sync_example::require(
        sync_example::kv_or_throw(node.conn, shared, key,
                                  "shared key") == value,
        "mesh value mismatch");
}

void close_node(NodeDb& node) {
    node.engine.reset();
    sync_example::disconnect_and_cleanup(node.conn, node.path);
}

} // namespace

int main() {
    const std::uint8_t logical_db_seed = 0xD4;
    const std::uint8_t node_a_seed = 0xA0;
    const std::uint8_t node_b_seed = 0xB0;
    const std::uint8_t node_c_seed = 0xC0;
    const mdbxc::sync::DbId db_id =
        sync_example::make_node(logical_db_seed);
    NodeDb a = { "A", "sync_05_node_a.mdbx",
                 sync_example::make_node(node_a_seed) };
    NodeDb b = { "B", "sync_05_node_b.mdbx",
                 sync_example::make_node(node_b_seed) };
    NodeDb c = { "C", "sync_05_node_c.mdbx",
                 sync_example::make_node(node_c_seed) };

    sync_example::cleanup(a.path);
    sync_example::cleanup(b.path);
    sync_example::cleanup(c.path);

    try {
        open_node(a, db_id);
        open_node(b, db_id);
        open_node(c, db_id);

        write_local(a, 1, "node A local write");
        write_local(b, 2, "node B local write");
        write_local(c, 3, "node C local write");

        // Full pairwise exchange. A chain such as A->B then B->C is not
        // enough because B does not forward A's remote-origin batch.
        sync_pair(a, b, db_id);
        sync_pair(a, c, db_id);
        sync_pair(b, a, db_id);
        sync_pair(b, c, db_id);
        sync_pair(c, a, db_id);
        sync_pair(c, b, db_id);

        require_shared(a, 1, "node A local write");
        require_shared(a, 2, "node B local write");
        require_shared(a, 3, "node C local write");

        require_shared(b, 1, "node A local write");
        require_shared(b, 2, "node B local write");
        require_shared(b, 3, "node C local write");

        require_shared(c, 1, "node A local write");
        require_shared(c, 2, "node B local write");
        require_shared(c, 3, "node C local write");

        close_node(a);
        close_node(b);
        close_node(c);
        std::printf("OK: sync_05_three_node_mesh\n");
        return 0;
    } catch (const std::exception& e) {
        std::printf("FAIL: %s\n", e.what());
        sync_example::cleanup(a.path);
        sync_example::cleanup(b.path);
        sync_example::cleanup(c.path);
        return 1;
    }
}
