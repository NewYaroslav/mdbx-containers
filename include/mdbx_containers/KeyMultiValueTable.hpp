#pragma once
#ifndef _MDBX_CONTAINERS_KEY_MULTI_VALUE_TABLE_HPP_INCLUDED
#define _MDBX_CONTAINERS_KEY_MULTI_VALUE_TABLE_HPP_INCLUDED

/// \file KeyMultiValueTable.hpp
/// \brief Multimap-like table storing multiple values per key.
/// \details
/// Stores every inserted key-value pair as a separate record, including
/// repeated identical \c (key,value) pairs. Values are returned in insertion
/// order within the same key. The default retrieval container is
/// \c std::multimap; use \c retrieve_all_vector() when every physical pair must
/// remain visible as a separate element regardless of container insertion rules.
/// Bulk reconciliation compares pair multiplicity and updates only the surplus
/// or missing records.

#include "common.hpp"
#include <limits>
#include <map>

namespace mdbxc {

    /// \class KeyMultiValueTable
    /// \ingroup mdbxc_tables
    /// \brief Multi-value table persisted in MDBX.
    /// \tparam KeyT Type of the keys.
    /// \tparam ValueT Type of the values.
    /// \tparam Options Compile-time table policy. Does not change the database
    ///         storage format.
    /// \details
    /// Provides a \c std::multimap -like API over an MDBX \c MDBX_DUPSORT table.
    /// Each insert creates a separate stored pair. Exact repeated
    /// \c (key,value) pairs are preserved by adding an internal sequence prefix
    /// to the stored duplicate value and removing that prefix while reading.
    ///
    /// Retrieval into \c std::multimap or \c std::vector<std::pair<KeyT,ValueT>>
    /// preserves multiple values per key. Retrieval into unique-key containers,
    /// such as \c std::map, is limited by that container's own insertion rules.
    ///
    /// \note Bulk reconciliation through \c reconcile() and \c operator=
    ///       preserves existing matching records, removes surplus records, and
    ///       inserts missing records. It does not reorder existing records to
    ///       match source iteration order.
    template<class KeyT, class ValueT, class Options = DefaultTableOptions>
    class KeyMultiValueTable final : public BaseTable {
    public:
        typedef std::pair<KeyT, ValueT> value_type;

        /// \brief Constructs table using existing connection.
        /// \param connection Existing connection.
        /// \param name Name of the table within the MDBX environment.
        /// \param flags Additional MDBX database flags.
        KeyMultiValueTable(std::shared_ptr<Connection> connection,
                           std::string name = "multi_value_store",
                           MDBX_db_flags_t flags = MDBX_DB_DEFAULTS | MDBX_CREATE)
            : BaseTable(std::move(connection),
                        std::move(name),
                        flags | MDBX_DUPSORT | get_mdbx_flags<KeyT>()) {}

        /// \brief Constructs table using configuration.
        /// \param config Configuration settings.
        /// \param name Name of the table within the MDBX environment.
        /// \param flags Additional MDBX database flags.
        explicit KeyMultiValueTable(const Config& config,
                                    std::string name = "multi_value_store",
                                    MDBX_db_flags_t flags = MDBX_DB_DEFAULTS | MDBX_CREATE)
            : BaseTable(Connection::create(config),
                        std::move(name),
                        flags | MDBX_DUPSORT | get_mdbx_flags<KeyT>()) {}

        /// \brief Destructor.
        ~KeyMultiValueTable() override = default;

        /// \brief Replaces table content with key-value pairs from a container.
        /// \tparam ContainerT Container type storing pairs.
        /// \param container Source pairs.
        /// \return Reference to this table.
        /// \note Equivalent to \c reconcile(container): the table is synchronized
        ///       to the source pair multiset.
        template<template<class...> class ContainerT>
        KeyMultiValueTable& operator=(const ContainerT<KeyT, ValueT>& container) {
            reconcile(container);
            return *this;
        }

        /// \brief Replaces table content with key-value pairs from a vector.
        /// \param container Source pairs.
        /// \return Reference to this table.
        /// \note Equivalent to \c reconcile(container): the table is synchronized
        ///       to the source pair multiset.
        KeyMultiValueTable& operator=(const std::vector<value_type>& container) {
            reconcile(container);
            return *this;
        }

        /// \brief Loads all key-value pairs into a container.
        /// \tparam ContainerT Container type storing pairs.
        /// \return Filled container.
        /// \note The default \c std::multimap preserves multiple values for one
        ///       key. Containers with unique keys, such as \c std::map, keep
        ///       only what their own insertion rules allow.
        template<template<class...> class ContainerT = std::multimap>
        ContainerT<KeyT, ValueT> operator()() const {
            return retrieve_all<ContainerT>();
        }

        /// \brief Loads pairs from the database into a container.
        /// \tparam ContainerT Container type storing pairs.
        /// \param container Output container.
        /// \param txn Optional transaction handle.
        template<template<class...> class ContainerT>
        void load(ContainerT<KeyT, ValueT>& container, MDBX_txn* txn = nullptr) const {
            with_transaction([this, &container](MDBX_txn* t) {
                db_load(container, t);
            }, TransactionMode::READ_ONLY, txn);
        }

        /// \brief Loads pairs from the database into a container.
        /// \tparam ContainerT Container type storing pairs.
        /// \param container Output container.
        /// \param txn Active transaction wrapper.
        template<template<class...> class ContainerT>
        void load(ContainerT<KeyT, ValueT>& container, const Transaction& txn) const {
            load(container, txn.handle());
        }

        /// \brief Loads pairs from the database into a vector.
        /// \param container Output vector.
        /// \param txn Optional transaction handle.
        /// \note Every stored pair is appended to the vector, including
        ///       repeated identical \c (key,value) pairs.
        void load(std::vector<value_type>& container, MDBX_txn* txn = nullptr) const {
            with_transaction([this, &container](MDBX_txn* t) {
                db_load(container, t);
            }, TransactionMode::READ_ONLY, txn);
        }

        /// \brief Loads pairs from the database into a vector.
        /// \param container Output vector.
        /// \param txn Active transaction wrapper.
        /// \note Every stored pair is appended to the vector, including
        ///       repeated identical \c (key,value) pairs.
        void load(std::vector<value_type>& container, const Transaction& txn) const {
            load(container, txn.handle());
        }

        /// \brief Retrieves all pairs into the requested container.
        /// \tparam ContainerT Container type storing pairs.
        /// \param txn Optional transaction handle.
        /// \return Filled container.
        /// \note The default \c std::multimap preserves multiple values for one
        ///       key. Containers with unique keys, such as \c std::map, keep
        ///       only what their own insertion rules allow.
        template<template<class...> class ContainerT = std::multimap>
        ContainerT<KeyT, ValueT> retrieve_all(MDBX_txn* txn = nullptr) const {
            ContainerT<KeyT, ValueT> container;
            load(container, txn);
            return container;
        }

        /// \brief Retrieves all pairs into the requested container.
        /// \tparam ContainerT Container type storing pairs.
        /// \param txn Active transaction wrapper.
        /// \return Filled container.
        /// \note The default \c std::multimap preserves multiple values for one
        ///       key. Containers with unique keys, such as \c std::map, keep
        ///       only what their own insertion rules allow.
        template<template<class...> class ContainerT = std::multimap>
        ContainerT<KeyT, ValueT> retrieve_all(const Transaction& txn) const {
            return retrieve_all<ContainerT>(txn.handle());
        }

        /// \brief Retrieves all pairs into a vector.
        /// \param txn Optional transaction handle.
        /// \return Vector containing all pairs.
        /// \note Preserves every stored pair as a separate vector element,
        ///       including repeated identical \c (key,value) pairs.
        std::vector<value_type> retrieve_all_vector(MDBX_txn* txn = nullptr) const {
            std::vector<value_type> container;
            load(container, txn);
            return container;
        }

        /// \brief Retrieves all pairs into a vector.
        /// \param txn Active transaction wrapper.
        /// \return Vector containing all pairs.
        /// \note Preserves every stored pair as a separate vector element,
        ///       including repeated identical \c (key,value) pairs.
        std::vector<value_type> retrieve_all_vector(const Transaction& txn) const {
            return retrieve_all_vector(txn.handle());
        }

        /// \brief Retrieves key-value pairs within an inclusive key range.
        /// \param from_key Start key in MDBX key order.
        /// \param to_key End key in MDBX key order.
        /// \param txn Optional transaction handle.
        /// \return Vector of key-value pairs in MDBX key order.
        /// \throws MdbxException if a database error occurs.
        /// \complexity O(log n + m), where m is the number of returned pairs.
        std::vector<value_type> range(const KeyT& from_key, const KeyT& to_key,
                                      MDBX_txn* txn = nullptr) const {
            std::vector<value_type> pairs;
            with_transaction([this, &from_key, &to_key, &pairs](MDBX_txn* t) {
                db_range(from_key, to_key, pairs, t);
            }, TransactionMode::READ_ONLY, txn);
            return pairs;
        }

        /// \brief Retrieves key-value pairs within an inclusive key range.
        /// \param from_key Start key in MDBX key order.
        /// \param to_key End key in MDBX key order.
        /// \param txn Active transaction wrapper.
        /// \return Vector of key-value pairs in MDBX key order.
        /// \throws MdbxException if a database error occurs.
        /// \complexity O(log n + m), where m is the number of returned pairs.
        std::vector<value_type> range(const KeyT& from_key, const KeyT& to_key,
                                      const Transaction& txn) const {
            return range(from_key, to_key, txn.handle());
        }

        /// \brief Retrieves values whose keys are within an inclusive key range.
        /// \param from_key Start key in MDBX key order.
        /// \param to_key End key in MDBX key order.
        /// \param txn Optional transaction handle.
        /// \return Vector of values in MDBX key order.
        /// \throws MdbxException if a database error occurs.
        /// \complexity O(log n + m), where m is the number of returned values.
        std::vector<ValueT> range_values(const KeyT& from_key, const KeyT& to_key,
                                         MDBX_txn* txn = nullptr) const {
            std::vector<ValueT> values;
            with_transaction([this, &from_key, &to_key, &values](MDBX_txn* t) {
                db_range_values(from_key, to_key, values, t);
            }, TransactionMode::READ_ONLY, txn);
            return values;
        }

        /// \brief Retrieves values whose keys are within an inclusive key range.
        /// \param from_key Start key in MDBX key order.
        /// \param to_key End key in MDBX key order.
        /// \param txn Active transaction wrapper.
        /// \return Vector of values in MDBX key order.
        /// \throws MdbxException if a database error occurs.
        /// \complexity O(log n + m), where m is the number of returned values.
        std::vector<ValueT> range_values(const KeyT& from_key, const KeyT& to_key,
                                         const Transaction& txn) const {
            return range_values(from_key, to_key, txn.handle());
        }

        /// \brief Appends pairs to the table.
        /// \tparam ContainerT Container type storing pairs.
        /// \param container Source pairs.
        /// \param txn Optional transaction handle.
        template<template<class...> class ContainerT>
        void append(const ContainerT<KeyT, ValueT>& container, MDBX_txn* txn = nullptr) {
            with_transaction([this, &container](MDBX_txn* t) {
                db_append(container, t);
            }, TransactionMode::WRITABLE, txn);
        }

        /// \brief Appends pairs to the table.
        /// \tparam ContainerT Container type storing pairs.
        /// \param container Source pairs.
        /// \param txn Active transaction wrapper.
        template<template<class...> class ContainerT>
        void append(const ContainerT<KeyT, ValueT>& container, const Transaction& txn) {
            append(container, txn.handle());
        }

        /// \brief Appends pairs from a vector to the table.
        /// \param container Source pairs.
        /// \param txn Optional transaction handle.
        void append(const std::vector<value_type>& container, MDBX_txn* txn = nullptr) {
            with_transaction([this, &container](MDBX_txn* t) {
                db_append(container, t);
            }, TransactionMode::WRITABLE, txn);
        }

        /// \brief Appends pairs from a vector to the table.
        /// \param container Source pairs.
        /// \param txn Active transaction wrapper.
        void append(const std::vector<value_type>& container, const Transaction& txn) {
            append(container, txn.handle());
        }

        /// \brief Replaces table content with pairs from a container.
        /// \tparam ContainerT Container type storing pairs.
        /// \param container Source pairs.
        /// \param txn Optional transaction handle.
        /// \note Synchronizes by pair multiplicity. Existing matching pairs are
        ///       kept, surplus pairs are removed, and missing pairs are inserted
        ///       in source iteration order. Repeated pairs are preserved only if
        ///       the source container itself preserves them, such as
        ///       \c std::multimap.
        template<template<class...> class ContainerT>
        void reconcile(const ContainerT<KeyT, ValueT>& container, MDBX_txn* txn = nullptr) {
            with_transaction([this, &container](MDBX_txn* t) {
                db_reconcile(container, t);
            }, TransactionMode::WRITABLE, txn);
        }

        /// \brief Replaces table content with pairs from a container.
        /// \tparam ContainerT Container type storing pairs.
        /// \param container Source pairs.
        /// \param txn Active transaction wrapper.
        /// \note Synchronizes by pair multiplicity. Existing matching pairs are
        ///       kept, surplus pairs are removed, and missing pairs are inserted
        ///       in source iteration order.
        template<template<class...> class ContainerT>
        void reconcile(const ContainerT<KeyT, ValueT>& container, const Transaction& txn) {
            reconcile(container, txn.handle());
        }

        /// \brief Replaces table content with pairs from a vector.
        /// \param container Source pairs.
        /// \param txn Optional transaction handle.
        /// \note Synchronizes by pair multiplicity. Existing matching pairs are
        ///       kept, surplus pairs are removed, and missing pairs are inserted
        ///       in vector order. Repeated identical \c (key,value) elements are
        ///       preserved as separate stored pairs.
        void reconcile(const std::vector<value_type>& container, MDBX_txn* txn = nullptr) {
            with_transaction([this, &container](MDBX_txn* t) {
                db_reconcile(container, t);
            }, TransactionMode::WRITABLE, txn);
        }

        /// \brief Replaces table content with pairs from a vector.
        /// \param container Source pairs.
        /// \param txn Active transaction wrapper.
        /// \note Synchronizes by pair multiplicity. Existing matching pairs are
        ///       kept, surplus pairs are removed, and missing pairs are inserted
        ///       in vector order.
        void reconcile(const std::vector<value_type>& container, const Transaction& txn) {
            reconcile(container, txn.handle());
        }

        /// \brief Inserts a key-value pair.
        /// \param key Key to insert.
        /// \param value Value to insert.
        /// \param txn Optional transaction handle.
        void insert(const KeyT& key, const ValueT& value, MDBX_txn* txn = nullptr) {
            with_transaction([this, &key, &value](MDBX_txn* t) {
                db_insert(key, value, t);
            }, TransactionMode::WRITABLE, txn);
        }

        /// \brief Inserts a key-value pair.
        /// \param key Key to insert.
        /// \param value Value to insert.
        /// \param txn Active transaction wrapper.
        void insert(const KeyT& key, const ValueT& value, const Transaction& txn) {
            insert(key, value, txn.handle());
        }

        /// \brief Inserts a key-value pair.
        /// \param pair Pair to insert.
        /// \param txn Optional transaction handle.
        void insert(const value_type& pair, MDBX_txn* txn = nullptr) {
            insert(pair.first, pair.second, txn);
        }

        /// \brief Inserts a key-value pair.
        /// \param pair Pair to insert.
        /// \param txn Active transaction wrapper.
        void insert(const value_type& pair, const Transaction& txn) {
            insert(pair, txn.handle());
        }

        /// \brief Finds all values for a key.
        /// \param key Key to search for.
        /// \param txn Optional transaction handle.
        /// \return Values stored for the key in insertion order.
        std::vector<ValueT> find(const KeyT& key, MDBX_txn* txn = nullptr) const {
            std::vector<ValueT> values;
            with_transaction([this, &key, &values](MDBX_txn* t) {
                db_find(key, values, t);
            }, TransactionMode::READ_ONLY, txn);
            return values;
        }

        /// \brief Finds all values for a key.
        /// \param key Key to search for.
        /// \param txn Active transaction wrapper.
        /// \return Values stored for the key in insertion order.
        std::vector<ValueT> find(const KeyT& key, const Transaction& txn) const {
            return find(key, txn.handle());
        }

        /// \brief Checks whether a key exists.
        /// \param key Key to look up.
        /// \param txn Optional transaction handle.
        /// \return \c true if the key exists.
        bool contains(const KeyT& key, MDBX_txn* txn = nullptr) const {
            bool res = false;
            with_transaction([this, &key, &res](MDBX_txn* t) {
                res = db_contains_key(key, t);
            }, TransactionMode::READ_ONLY, txn);
            return res;
        }

        /// \brief Checks whether a key exists.
        /// \param key Key to look up.
        /// \param txn Active transaction wrapper.
        /// \return \c true if the key exists.
        bool contains(const KeyT& key, const Transaction& txn) const {
            return contains(key, txn.handle());
        }

        /// \brief Checks whether a key-value pair exists.
        /// \param key Key to look up.
        /// \param value Value to match.
        /// \param txn Optional transaction handle.
        /// \return \c true if a matching pair exists.
        bool contains(const KeyT& key, const ValueT& value, MDBX_txn* txn = nullptr) const {
            bool res = false;
            with_transaction([this, &key, &value, &res](MDBX_txn* t) {
                res = db_contains_pair(key, value, t);
            }, TransactionMode::READ_ONLY, txn);
            return res;
        }

        /// \brief Checks whether a key-value pair exists.
        /// \param key Key to look up.
        /// \param value Value to match.
        /// \param txn Active transaction wrapper.
        /// \return \c true if a matching pair exists.
        bool contains(const KeyT& key, const ValueT& value, const Transaction& txn) const {
            return contains(key, value, txn.handle());
        }

        /// \brief Counts all stored key-value pairs.
        /// \param txn Optional transaction handle.
        /// \return Number of pairs in the table.
        std::size_t count(MDBX_txn* txn = nullptr) const {
            std::size_t res = 0;
            with_transaction([this, &res](MDBX_txn* t) {
                res = db_count(t);
            }, TransactionMode::READ_ONLY, txn);
            return res;
        }

        /// \brief Counts all stored key-value pairs.
        /// \param txn Active transaction wrapper.
        /// \return Number of pairs in the table.
        std::size_t count(const Transaction& txn) const {
            return count(txn.handle());
        }

        /// \brief Counts values stored for a key.
        /// \param key Key to count.
        /// \param txn Optional transaction handle.
        /// \return Number of values for the key.
        std::size_t count(const KeyT& key, MDBX_txn* txn = nullptr) const {
            std::size_t res = 0;
            with_transaction([this, &key, &res](MDBX_txn* t) {
                res = db_count_key(key, t);
            }, TransactionMode::READ_ONLY, txn);
            return res;
        }

        /// \brief Counts values stored for a key.
        /// \param key Key to count.
        /// \param txn Active transaction wrapper.
        /// \return Number of values for the key.
        std::size_t count(const KeyT& key, const Transaction& txn) const {
            return count(key, txn.handle());
        }

        /// \brief Counts exact value matches under a key.
        /// \param key Key to count.
        /// \param value Value to match.
        /// \param txn Optional transaction handle.
        /// \return Number of matching pairs.
        std::size_t count(const KeyT& key, const ValueT& value, MDBX_txn* txn = nullptr) const {
            std::size_t res = 0;
            with_transaction([this, &key, &value, &res](MDBX_txn* t) {
                res = db_count_pair(key, value, t);
            }, TransactionMode::READ_ONLY, txn);
            return res;
        }

        /// \brief Counts exact value matches under a key.
        /// \param key Key to count.
        /// \param value Value to match.
        /// \param txn Active transaction wrapper.
        /// \return Number of matching pairs.
        std::size_t count(const KeyT& key, const ValueT& value, const Transaction& txn) const {
            return count(key, value, txn.handle());
        }

        /// \brief Checks whether the table has no pairs.
        /// \param txn Optional transaction handle.
        /// \return \c true if the table is empty.
        bool empty(MDBX_txn* txn = nullptr) const {
            return count(txn) == 0;
        }

        /// \brief Checks whether the table has no pairs.
        /// \param txn Active transaction wrapper.
        /// \return \c true if the table is empty.
        bool empty(const Transaction& txn) const {
            return empty(txn.handle());
        }

        /// \brief Erases all values for a key.
        /// \param key Key to remove.
        /// \param txn Optional transaction handle.
        /// \return \c true if the key was removed.
        bool erase(const KeyT& key, MDBX_txn* txn = nullptr) {
            bool res = false;
            with_transaction([this, &key, &res](MDBX_txn* t) {
                res = db_erase_key(key, t);
            }, TransactionMode::WRITABLE, txn);
            return res;
        }

        /// \brief Erases all values for a key.
        /// \param key Key to remove.
        /// \param txn Active transaction wrapper.
        /// \return \c true if the key was removed.
        bool erase(const KeyT& key, const Transaction& txn) {
            return erase(key, txn.handle());
        }

        /// \brief Erases all exact value matches under a key.
        /// \param key Key to remove from.
        /// \param value Value to remove.
        /// \param txn Optional transaction handle.
        /// \return Number of removed pairs.
        std::size_t erase(const KeyT& key, const ValueT& value, MDBX_txn* txn = nullptr) {
            std::size_t removed = 0;
            with_transaction([this, &key, &value, &removed](MDBX_txn* t) {
                removed = db_erase_pair(key, value, t);
            }, TransactionMode::WRITABLE, txn);
            return removed;
        }

        /// \brief Erases all exact value matches under a key.
        /// \param key Key to remove from.
        /// \param value Value to remove.
        /// \param txn Active transaction wrapper.
        /// \return Number of removed pairs.
        std::size_t erase(const KeyT& key, const ValueT& value, const Transaction& txn) {
            return erase(key, value, txn.handle());
        }

        /// \brief Removes all pairs.
        /// \param txn Optional transaction handle.
        void clear(MDBX_txn* txn = nullptr) {
            with_transaction([this](MDBX_txn* t) {
                db_clear(t);
            }, TransactionMode::WRITABLE, txn);
        }

        /// \brief Removes all pairs.
        /// \param txn Active transaction wrapper.
        void clear(const Transaction& txn) {
            clear(txn.handle());
        }

    private:
        static const std::size_t sequence_size = sizeof(uint64_t);

        struct SerializedPairKey {
            std::vector<uint8_t> key;
            std::vector<uint8_t> value;

            bool operator<(const SerializedPairKey& other) const {
                if (key < other.key) return true;
                if (other.key < key) return false;
                return value < other.value;
            }
        };

        struct ReconcileEntry {
            value_type pair;
            SerializedPairKey serialized;

            ReconcileEntry(const value_type& p, const SerializedPairKey& s)
                : pair(p), serialized(s) {}
        };

        typedef std::map<SerializedPairKey, std::size_t> PairCountMap;

        template<typename F>
        void with_transaction(F&& action, TransactionMode mode, MDBX_txn* txn = nullptr) const {
            if (txn) {
                action(txn);
                return;
            }
            txn = thread_txn();
            if (txn) {
                action(txn);
                return;
            }

            auto txn_guard = m_connection->transaction(mode);
            try {
                action(txn_guard.handle());
                txn_guard.commit();
            } catch (...) {
                try { txn_guard.rollback(); } catch (...) {}
                throw;
            }
        }

        static void encode_sequence(uint64_t sequence, uint8_t* out) {
            for (std::size_t i = 0; i < sequence_size; ++i) {
                out[i] = static_cast<uint8_t>((sequence >> ((sequence_size - 1 - i) * 8)) & 0xffu);
            }
        }

        static uint64_t decode_sequence(const MDBX_val& stored) {
            if (stored.iov_len < sequence_size) {
                throw std::runtime_error("Corrupted multi-value record");
            }
            const uint8_t* bytes = static_cast<const uint8_t*>(stored.iov_base);
            uint64_t sequence = 0;
            for (std::size_t i = 0; i < sequence_size; ++i) {
                sequence = (sequence << 8) | static_cast<uint64_t>(bytes[i]);
            }
            return sequence;
        }

        static MDBX_val strip_sequence(const MDBX_val& stored) {
            if (stored.iov_len < sequence_size) {
                throw std::runtime_error("Corrupted multi-value record");
            }
            MDBX_val raw;
            raw.iov_base = static_cast<uint8_t*>(stored.iov_base) + sequence_size;
            raw.iov_len = stored.iov_len - sequence_size;
            return raw;
        }

        static std::vector<uint8_t> copy_bytes(const MDBX_val& value) {
            std::vector<uint8_t> bytes(value.iov_len);
            if (value.iov_len) {
                std::memcpy(bytes.data(), value.iov_base, value.iov_len);
            }
            return bytes;
        }

        SerializedPairKey make_serialized_pair_key(const KeyT& key, const ValueT& value) const {
            SerializeScratch sc_key;
            SerializeScratch sc_value;
            MDBX_val db_key = serialize_key<Options::safe_integer_key>(key, sc_key);
            MDBX_val raw_value = make_comparable_value(value, sc_value);

            SerializedPairKey out;
            out.key = copy_bytes(db_key);
            out.value = copy_bytes(raw_value);
            return out;
        }

        SerializedPairKey make_serialized_pair_key(const MDBX_val& db_key, const MDBX_val& db_value) const {
            SerializedPairKey out;
            out.key = copy_bytes(db_key);
            out.value = copy_bytes(strip_sequence(db_value));
            return out;
        }

        MDBX_val make_stored_value(uint64_t sequence, const ValueT& value, SerializeScratch& sc_value) const {
            SerializeScratch raw_scratch;
            MDBX_val raw_value = serialize_value(value, raw_scratch);
            sc_value.bytes.resize(sequence_size + raw_value.iov_len);
            encode_sequence(sequence, sc_value.bytes.data());
            if (raw_value.iov_len) {
                std::memcpy(sc_value.bytes.data() + sequence_size, raw_value.iov_base, raw_value.iov_len);
            }
            return sc_value.view_bytes();
        }

        MDBX_val make_comparable_value(const ValueT& value, SerializeScratch& sc_value) const {
            SerializeScratch raw_scratch;
            MDBX_val raw_value = serialize_value(value, raw_scratch);
            sc_value.bytes.resize(raw_value.iov_len);
            if (raw_value.iov_len) {
                std::memcpy(sc_value.bytes.data(), raw_value.iov_base, raw_value.iov_len);
            }
            return sc_value.view_bytes();
        }

        bool stored_value_matches(const MDBX_val& stored, const MDBX_val& raw_value) const {
            MDBX_val stored_raw = strip_sequence(stored);
            if (stored_raw.iov_len != raw_value.iov_len) {
                return false;
            }
            if (stored_raw.iov_len == 0) {
                return true;
            }
            return std::memcmp(stored_raw.iov_base, raw_value.iov_base, stored_raw.iov_len) == 0;
        }

        uint64_t next_sequence(const KeyT& key, MDBX_txn* txn) const {
            MDBX_cursor* cursor = nullptr;
            check_mdbx(mdbx_cursor_open(txn, m_dbi, &cursor), "Failed to open MDBX cursor");
            try {
                SerializeScratch sc_key;
                MDBX_val db_key = serialize_key<Options::safe_integer_key>(key, sc_key);
                MDBX_val db_val;
                int rc = mdbx_cursor_get(cursor, &db_key, &db_val, MDBX_SET_KEY);
                if (rc == MDBX_NOTFOUND) {
                    mdbx_cursor_close(cursor);
                    return 0;
                }
                check_mdbx(rc, "Failed to seek key");
                rc = mdbx_cursor_get(cursor, &db_key, &db_val, MDBX_LAST_DUP);
                check_mdbx(rc, "Failed to seek last value");
                uint64_t last = decode_sequence(db_val);
                if (last == std::numeric_limits<uint64_t>::max()) {
                    throw std::overflow_error("Per-key multi-value sequence exhausted");
                }
                mdbx_cursor_close(cursor);
                return last + 1;
            } catch (...) {
                mdbx_cursor_close(cursor);
                throw;
            }
        }

        template<template<class...> class ContainerT>
        void db_load(ContainerT<KeyT, ValueT>& container, MDBX_txn* txn) const {
            MDBX_cursor* cursor = nullptr;
            check_mdbx(mdbx_cursor_open(txn, m_dbi, &cursor), "Failed to open MDBX cursor");
            try {
                MDBX_val db_key, db_val;
                int rc = MDBX_SUCCESS;
                while ((rc = mdbx_cursor_get(cursor, &db_key, &db_val, MDBX_NEXT)) == MDBX_SUCCESS) {
                    KeyT key = deserialize_key<KeyT>(db_key);
                    ValueT value = deserialize_value<ValueT>(strip_sequence(db_val));
                    container.emplace(std::move(key), std::move(value));
                }
                if (rc != MDBX_NOTFOUND) {
                    check_mdbx(rc, "Failed to read multi-value table");
                }
                mdbx_cursor_close(cursor);
            } catch (...) {
                mdbx_cursor_close(cursor);
                throw;
            }
        }

        void db_load(std::vector<value_type>& container, MDBX_txn* txn) const {
            MDBX_cursor* cursor = nullptr;
            check_mdbx(mdbx_cursor_open(txn, m_dbi, &cursor), "Failed to open MDBX cursor");
            try {
                MDBX_val db_key, db_val;
                int rc = MDBX_SUCCESS;
                while ((rc = mdbx_cursor_get(cursor, &db_key, &db_val, MDBX_NEXT)) == MDBX_SUCCESS) {
                    KeyT key = deserialize_key<KeyT>(db_key);
                    ValueT value = deserialize_value<ValueT>(strip_sequence(db_val));
                    container.emplace_back(std::move(key), std::move(value));
                }
                if (rc != MDBX_NOTFOUND) {
                    check_mdbx(rc, "Failed to read multi-value table");
                }
                mdbx_cursor_close(cursor);
            } catch (...) {
                mdbx_cursor_close(cursor);
                throw;
            }
        }

        void db_range(const KeyT& from_key, const KeyT& to_key,
                      std::vector<value_type>& pairs, MDBX_txn* txn) const {
            SerializeScratch sc_from_key;
            SerializeScratch sc_to_key;
            MDBX_val db_from_key = serialize_key<Options::safe_integer_key>(from_key, sc_from_key);
            MDBX_val db_to_key = serialize_key<Options::safe_integer_key>(to_key, sc_to_key);
            if (mdbx_cmp(txn, m_dbi, &db_from_key, &db_to_key) > 0) return;

            CursorGuard cursor;
            check_mdbx(mdbx_cursor_open(txn, m_dbi, cursor.out()), "Failed to open MDBX cursor");

            MDBX_val db_key = db_from_key;
            MDBX_val db_val;
            bool stopped_by_upper_bound = false;
            int rc = mdbx_cursor_get(cursor.get(), &db_key, &db_val, MDBX_SET_RANGE);
            while (rc == MDBX_SUCCESS) {
                if (mdbx_cmp(txn, m_dbi, &db_key, &db_to_key) > 0) {
                    stopped_by_upper_bound = true;
                    break;
                }
                KeyT key = deserialize_key<KeyT>(db_key);
                ValueT value = deserialize_value<ValueT>(strip_sequence(db_val));
                pairs.emplace_back(std::move(key), std::move(value));
                rc = mdbx_cursor_get(cursor.get(), &db_key, &db_val, MDBX_NEXT);
            }
            if (!stopped_by_upper_bound && rc != MDBX_NOTFOUND) {
                check_mdbx(rc, "Failed to read multi-value key range");
            }
        }

        void db_range_values(const KeyT& from_key, const KeyT& to_key,
                             std::vector<ValueT>& values, MDBX_txn* txn) const {
            SerializeScratch sc_from_key;
            SerializeScratch sc_to_key;
            MDBX_val db_from_key = serialize_key<Options::safe_integer_key>(from_key, sc_from_key);
            MDBX_val db_to_key = serialize_key<Options::safe_integer_key>(to_key, sc_to_key);
            if (mdbx_cmp(txn, m_dbi, &db_from_key, &db_to_key) > 0) return;

            CursorGuard cursor;
            check_mdbx(mdbx_cursor_open(txn, m_dbi, cursor.out()), "Failed to open MDBX cursor");

            MDBX_val db_key = db_from_key;
            MDBX_val db_val;
            bool stopped_by_upper_bound = false;
            int rc = mdbx_cursor_get(cursor.get(), &db_key, &db_val, MDBX_SET_RANGE);
            while (rc == MDBX_SUCCESS) {
                if (mdbx_cmp(txn, m_dbi, &db_key, &db_to_key) > 0) {
                    stopped_by_upper_bound = true;
                    break;
                }
                values.emplace_back(deserialize_value<ValueT>(strip_sequence(db_val)));
                rc = mdbx_cursor_get(cursor.get(), &db_key, &db_val, MDBX_NEXT);
            }
            if (!stopped_by_upper_bound && rc != MDBX_NOTFOUND) {
                check_mdbx(rc, "Failed to read multi-value key range values");
            }
        }

        template<template<class...> class ContainerT>
        void db_append(const ContainerT<KeyT, ValueT>& container, MDBX_txn* txn) {
            for (typename ContainerT<KeyT, ValueT>::const_iterator it = container.begin();
                 it != container.end(); ++it) {
                db_insert(it->first, it->second, txn);
            }
        }

        void db_append(const std::vector<value_type>& container, MDBX_txn* txn) {
            for (typename std::vector<value_type>::const_iterator it = container.begin();
                 it != container.end(); ++it) {
                db_insert(it->first, it->second, txn);
            }
        }

        template<template<class...> class ContainerT>
        void collect_reconcile_entries(const ContainerT<KeyT, ValueT>& container,
                                       std::vector<ReconcileEntry>& entries,
                                       PairCountMap& desired) const {
            for (typename ContainerT<KeyT, ValueT>::const_iterator it = container.begin();
                 it != container.end(); ++it) {
                value_type pair(it->first, it->second);
                SerializedPairKey serialized = make_serialized_pair_key(pair.first, pair.second);
                ++desired[serialized];
                entries.push_back(ReconcileEntry(pair, serialized));
            }
        }

        void collect_reconcile_entries(const std::vector<value_type>& container,
                                       std::vector<ReconcileEntry>& entries,
                                       PairCountMap& desired) const {
            for (typename std::vector<value_type>::const_iterator it = container.begin();
                 it != container.end(); ++it) {
                SerializedPairKey serialized = make_serialized_pair_key(it->first, it->second);
                ++desired[serialized];
                entries.push_back(ReconcileEntry(*it, serialized));
            }
        }

        template<class ContainerT>
        void db_reconcile_impl(const ContainerT& container, MDBX_txn* txn) {
            std::vector<ReconcileEntry> entries;
            PairCountMap desired;
            collect_reconcile_entries(container, entries, desired);

            PairCountMap kept;
            MDBX_cursor* cursor = nullptr;
            check_mdbx(mdbx_cursor_open(txn, m_dbi, &cursor), "Failed to open MDBX cursor");
            try {
                MDBX_val db_key, db_val;
                int rc = MDBX_SUCCESS;
                while ((rc = mdbx_cursor_get(cursor, &db_key, &db_val, MDBX_NEXT)) == MDBX_SUCCESS) {
                    SerializedPairKey serialized = make_serialized_pair_key(db_key, db_val);
                    typename PairCountMap::const_iterator desired_it = desired.find(serialized);
                    if (desired_it != desired.end() && kept[serialized] < desired_it->second) {
                        ++kept[serialized];
                    } else {
                        check_mdbx(mdbx_cursor_del(cursor, MDBX_CURRENT), "Failed to erase surplus record");
                    }
                }
                if (rc != MDBX_NOTFOUND) {
                    check_mdbx(rc, "Failed to scan multi-value table");
                }
                mdbx_cursor_close(cursor);
            } catch (...) {
                mdbx_cursor_close(cursor);
                throw;
            }

            PairCountMap inserted;
            for (typename std::vector<ReconcileEntry>::const_iterator it = entries.begin();
                 it != entries.end(); ++it) {
                std::size_t have = kept[it->serialized] + inserted[it->serialized];
                if (have < desired[it->serialized]) {
                    db_insert(it->pair.first, it->pair.second, txn);
                    ++inserted[it->serialized];
                }
            }
        }

        template<template<class...> class ContainerT>
        void db_reconcile(const ContainerT<KeyT, ValueT>& container, MDBX_txn* txn) {
            db_reconcile_impl(container, txn);
        }

        void db_reconcile(const std::vector<value_type>& container, MDBX_txn* txn) {
            db_reconcile_impl(container, txn);
        }

        void db_insert(const KeyT& key, const ValueT& value, MDBX_txn* txn) {
            SerializeScratch sc_key;
            SerializeScratch sc_value;
            uint64_t sequence = next_sequence(key, txn);
            MDBX_val db_key = serialize_key<Options::safe_integer_key>(key, sc_key);
            MDBX_val db_val = make_stored_value(sequence, value, sc_value);
            check_dupsort_value_size(db_val);
            check_mdbx(mdbx_put(txn, m_dbi, &db_key, &db_val, MDBX_UPSERT), "Failed to insert multi-value record");
        }

        void db_find(const KeyT& key, std::vector<ValueT>& values, MDBX_txn* txn) const {
            MDBX_cursor* cursor = nullptr;
            check_mdbx(mdbx_cursor_open(txn, m_dbi, &cursor), "Failed to open MDBX cursor");
            try {
                SerializeScratch sc_key;
                MDBX_val db_key = serialize_key<Options::safe_integer_key>(key, sc_key);
                MDBX_val db_val;
                int rc = mdbx_cursor_get(cursor, &db_key, &db_val, MDBX_SET_KEY);
                if (rc == MDBX_NOTFOUND) {
                    mdbx_cursor_close(cursor);
                    return;
                }
                check_mdbx(rc, "Failed to seek key");
                while (rc == MDBX_SUCCESS) {
                    values.emplace_back(deserialize_value<ValueT>(strip_sequence(db_val)));
                    rc = mdbx_cursor_get(cursor, &db_key, &db_val, MDBX_NEXT_DUP);
                }
                if (rc != MDBX_NOTFOUND) {
                    check_mdbx(rc, "Failed to read duplicate values");
                }
                mdbx_cursor_close(cursor);
            } catch (...) {
                mdbx_cursor_close(cursor);
                throw;
            }
        }

        bool db_contains_key(const KeyT& key, MDBX_txn* txn) const {
            SerializeScratch sc_key;
            MDBX_val db_key = serialize_key<Options::safe_integer_key>(key, sc_key);
            MDBX_val db_val;
            int rc = mdbx_get(txn, m_dbi, &db_key, &db_val);
            if (rc == MDBX_SUCCESS) return true;
            if (rc == MDBX_NOTFOUND) return false;
            check_mdbx(rc, "Failed to check key presence");
            return false;
        }

        bool db_contains_pair(const KeyT& key, const ValueT& value, MDBX_txn* txn) const {
            return db_count_pair(key, value, txn) > 0;
        }

        std::size_t db_count(MDBX_txn* txn) const {
            MDBX_stat stat;
            check_mdbx(mdbx_dbi_stat(txn, m_dbi, &stat, sizeof(stat)), "Failed to query database statistics");
            return stat.ms_entries;
        }

        std::size_t db_count_key(const KeyT& key, MDBX_txn* txn) const {
            MDBX_cursor* cursor = nullptr;
            check_mdbx(mdbx_cursor_open(txn, m_dbi, &cursor), "Failed to open MDBX cursor");
            try {
                SerializeScratch sc_key;
                MDBX_val db_key = serialize_key<Options::safe_integer_key>(key, sc_key);
                MDBX_val db_val;
                int rc = mdbx_cursor_get(cursor, &db_key, &db_val, MDBX_SET_KEY);
                if (rc == MDBX_NOTFOUND) {
                    mdbx_cursor_close(cursor);
                    return 0;
                }
                check_mdbx(rc, "Failed to seek key");
                size_t found = 0;
                check_mdbx(mdbx_cursor_count(cursor, &found), "Failed to count duplicate values");
                mdbx_cursor_close(cursor);
                return found;
            } catch (...) {
                mdbx_cursor_close(cursor);
                throw;
            }
        }

        std::size_t db_count_pair(const KeyT& key, const ValueT& value, MDBX_txn* txn) const {
            MDBX_cursor* cursor = nullptr;
            check_mdbx(mdbx_cursor_open(txn, m_dbi, &cursor), "Failed to open MDBX cursor");
            try {
                SerializeScratch sc_key;
                SerializeScratch sc_value;
                MDBX_val db_key = serialize_key<Options::safe_integer_key>(key, sc_key);
                MDBX_val raw_value = make_comparable_value(value, sc_value);
                MDBX_val db_val;
                int rc = mdbx_cursor_get(cursor, &db_key, &db_val, MDBX_SET_KEY);
                if (rc == MDBX_NOTFOUND) {
                    mdbx_cursor_close(cursor);
                    return 0;
                }
                check_mdbx(rc, "Failed to seek key");
                std::size_t found = 0;
                while (rc == MDBX_SUCCESS) {
                    if (stored_value_matches(db_val, raw_value)) {
                        ++found;
                    }
                    rc = mdbx_cursor_get(cursor, &db_key, &db_val, MDBX_NEXT_DUP);
                }
                if (rc != MDBX_NOTFOUND) {
                    check_mdbx(rc, "Failed to scan duplicate values");
                }
                mdbx_cursor_close(cursor);
                return found;
            } catch (...) {
                mdbx_cursor_close(cursor);
                throw;
            }
        }

        bool db_erase_key(const KeyT& key, MDBX_txn* txn) {
            SerializeScratch sc_key;
            MDBX_val db_key = serialize_key<Options::safe_integer_key>(key, sc_key);
            int rc = mdbx_del(txn, m_dbi, &db_key, nullptr);
            if (rc == MDBX_SUCCESS) return true;
            if (rc == MDBX_NOTFOUND) return false;
            check_mdbx(rc, "Failed to erase key");
            return false;
        }

        std::size_t db_erase_pair(const KeyT& key, const ValueT& value, MDBX_txn* txn) {
            MDBX_cursor* cursor = nullptr;
            check_mdbx(mdbx_cursor_open(txn, m_dbi, &cursor), "Failed to open MDBX cursor");
            try {
                SerializeScratch sc_key;
                SerializeScratch sc_value;
                MDBX_val db_key = serialize_key<Options::safe_integer_key>(key, sc_key);
                MDBX_val raw_value = make_comparable_value(value, sc_value);
                MDBX_val db_val;
                int rc = mdbx_cursor_get(cursor, &db_key, &db_val, MDBX_SET_KEY);
                if (rc == MDBX_NOTFOUND) {
                    mdbx_cursor_close(cursor);
                    return 0;
                }
                check_mdbx(rc, "Failed to seek key");
                std::size_t removed = 0;
                while (rc == MDBX_SUCCESS) {
                    bool should_remove = stored_value_matches(db_val, raw_value);
                    if (should_remove) {
                        check_mdbx(mdbx_cursor_del(cursor, MDBX_CURRENT), "Failed to erase duplicate value");
                        ++removed;
                    }
                    rc = mdbx_cursor_get(cursor, &db_key, &db_val, MDBX_NEXT_DUP);
                }
                if (rc != MDBX_NOTFOUND) {
                    check_mdbx(rc, "Failed to scan duplicate values");
                }
                mdbx_cursor_close(cursor);
                return removed;
            } catch (...) {
                mdbx_cursor_close(cursor);
                throw;
            }
        }

        void db_clear(MDBX_txn* txn) {
            check_mdbx(mdbx_drop(txn, m_dbi, 0), "Failed to clear table");
        }
    };

} // namespace mdbxc

#endif // _MDBX_CONTAINERS_KEY_MULTI_VALUE_TABLE_HPP_INCLUDED
