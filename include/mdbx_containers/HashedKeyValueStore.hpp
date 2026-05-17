#pragma once
#ifndef _MDBX_CONTAINERS_HASHED_KEY_VALUE_STORE_HPP_INCLUDED
#define _MDBX_CONTAINERS_HASHED_KEY_VALUE_STORE_HPP_INCLUDED

/// \file HashedKeyValueStore.hpp
/// \brief Hash-indexed key-value store for string and byte-vector keys.

#include "common.hpp"
#include "detail/ResultContainers.hpp"
#include "Hash.hpp"

#include <map>
#include <set>
#include <utility>
#include <vector>

namespace mdbxc {

    /// \class HashedKeyValueStore
    /// \ingroup mdbxc_tables
    /// \brief Map-like table with a hash index over string or byte-vector keys.
    /// \tparam KeyT Key type. Supported types are \c std::string,
    ///         \c std::vector<char>, \c std::vector<unsigned char>,
    ///         \c std::vector<uint8_t>, and in C++17 \c std::vector<std::byte>.
    /// \tparam ValueT Type of values stored under each key.
    /// \tparam Hasher Callable taking \ref ByteView and returning a 64-bit hash.
    ///
    /// \details
    /// Stores one value per original key, similar to \ref KeyValueTable, while
    /// looking up records through a 64-bit hash bucket. The stored original key
    /// bytes are always compared before a record is accepted, so correctness does
    /// not rely on hash uniqueness.
    ///
    /// The store uses one MDBX DBI opened with \c MDBX_INTEGERKEY and
    /// \c MDBX_DUPSORT. The MDBX key is a 64-bit hash of the original key bytes,
    /// and each duplicate value stores the original key bytes followed by the
    /// serialized value.
    ///
    /// The default \ref XXH3Hasher is non-cryptographic and intended only as a
    /// lookup accelerator. For externally controlled keys use a stable keyed
    /// hasher such as \ref SipHashHasher. Changing the hasher or keyed hasher
    /// material for an existing store changes the lookup domain.
    template<class KeyT, class ValueT, class Hasher = XXH3Hasher>
    class HashedKeyValueStore final : public BaseTable {
        static_assert(is_hashed_key_type<KeyT>::value,
                      "HashedKeyValueStore key must be std::string or a supported byte vector");

    public:
        typedef std::pair<KeyT, ValueT> value_type;

        /// \brief Constructs a store using an existing connection.
        /// \param connection Existing \ref Connection instance.
        /// \param name Name of the records table within the MDBX environment.
        /// \param hasher Hashing strategy used for key lookup.
        /// \param flags Additional MDBX database flags for table creation.
        explicit HashedKeyValueStore(std::shared_ptr<Connection> connection,
                                     std::string name = "hashed_kv_store",
                                     Hasher hasher = Hasher(),
                                     MDBX_db_flags_t flags = MDBX_DB_DEFAULTS | MDBX_CREATE)
            : BaseTable(std::move(connection), name, store_flags(flags)),
              m_hasher(std::move(hasher)) {}

        /// \brief Constructs a store using a database configuration.
        /// \param config Configuration settings for the database.
        /// \param name Name of the records table within the MDBX environment.
        /// \param hasher Hashing strategy used for key lookup.
        /// \param flags Additional MDBX database flags for table creation.
        explicit HashedKeyValueStore(const Config& config,
                                     std::string name = "hashed_kv_store",
                                     Hasher hasher = Hasher(),
                                     MDBX_db_flags_t flags = MDBX_DB_DEFAULTS | MDBX_CREATE)
            : BaseTable(Connection::create(config), name, store_flags(flags)),
              m_hasher(std::move(hasher)) {}

        /// \brief Destructor.
        ~HashedKeyValueStore() override = default;

        /// \brief Replaces table content with pairs from a container.
        /// \tparam ContainerT Container type storing key-value pairs.
        /// \param container Source pairs.
        /// \return Reference to this store.
        template<template<class...> class ContainerT>
        HashedKeyValueStore& operator=(const ContainerT<KeyT, ValueT>& container) {
            reconcile(container);
            return *this;
        }

        /// \brief Replaces table content with pairs from a vector.
        /// \param container Source pairs.
        /// \return Reference to this store.
        HashedKeyValueStore& operator=(const std::vector<value_type>& container) {
            reconcile(container);
            return *this;
        }

        /// \brief Retrieves all pairs into the requested container type.
        /// \tparam ContainerT Container type, defaulting to \c std::map.
        /// \return Filled container.
        template<template<class...> class ContainerT = std::map>
        typename detail::key_value_result_container<ContainerT, KeyT, ValueT>::type operator()() const {
            return retrieve_all<ContainerT>();
        }

        /// \brief Helper proxy for assignment through \c operator[].
        class AssignmentProxy {
        public:
            /// \brief Constructs an assignment proxy.
            /// \param store Owning store.
            /// \param key Key associated with the proxy.
            AssignmentProxy(HashedKeyValueStore& store, KeyT key)
                : m_store(store), m_key(std::move(key)) {}

            /// \brief Assigns a value to the key.
            /// \param value Value to store.
            /// \return Reference to this proxy.
            AssignmentProxy& operator=(const ValueT& value) {
                m_store.insert_or_assign(m_key, value);
                return *this;
            }

            /// \brief Reads the value, inserting a default value when missing.
            /// \return Stored or default-constructed value.
            /// \warning Reading a missing key through this conversion mutates
            ///          the store by persisting a default-constructed value.
            operator ValueT() const {
#if __cplusplus >= 201703L
                std::optional<ValueT> found = m_store.find(m_key);
                if (found) return *found;
#else
                std::pair<bool, ValueT> found = m_store.find_compat(m_key);
                if (found.first) return found.second;
#endif
                ValueT value{};
                m_store.insert_or_assign(m_key, value);
                return value;
            }

        private:
            HashedKeyValueStore& m_store;
            KeyT m_key;
        };

        /// \brief Provides convenient access to insert or read a value by key.
        /// \param key Key to access.
        /// \return Proxy object used for assignment or implicit read.
        AssignmentProxy operator[](const KeyT& key) {
            return AssignmentProxy(*this, key);
        }

        /// \brief Loads pairs from the store into a container.
        /// \tparam ContainerT Container type storing key-value pairs.
        /// \param container Output container.
        /// \param txn Optional transaction handle.
        template<template<class...> class ContainerT>
        void load(ContainerT<KeyT, ValueT>& container, MDBX_txn* txn = nullptr) const {
            with_transaction([this, &container](MDBX_txn* t) {
                db_load(container, t);
            }, TransactionMode::READ_ONLY, txn);
        }

        /// \brief Loads pairs from the store into a container.
        /// \tparam ContainerT Container type storing key-value pairs.
        /// \param container Output container.
        /// \param txn Active transaction wrapper.
        template<template<class...> class ContainerT>
        void load(ContainerT<KeyT, ValueT>& container, const Transaction& txn) const {
            load(container, txn.handle());
        }

        /// \brief Loads pairs from the store into a vector.
        /// \param container Output vector.
        /// \param txn Optional transaction handle.
        void load(std::vector<value_type>& container, MDBX_txn* txn = nullptr) const {
            with_transaction([this, &container](MDBX_txn* t) {
                db_load(container, t);
            }, TransactionMode::READ_ONLY, txn);
        }

        /// \brief Loads pairs from the store into a vector.
        /// \param container Output vector.
        /// \param txn Active transaction wrapper.
        void load(std::vector<value_type>& container, const Transaction& txn) const {
            load(container, txn.handle());
        }

        /// \brief Retrieves all pairs into the requested container.
        /// \tparam ContainerT Container type, defaulting to \c std::map.
        /// \param txn Optional transaction handle.
        /// \return Filled container.
        template<template<class...> class ContainerT = std::map>
        typename detail::key_value_result_container<ContainerT, KeyT, ValueT>::type
        retrieve_all(MDBX_txn* txn = nullptr) const {
            typename detail::key_value_result_container<ContainerT, KeyT, ValueT>::type container;
            load(container, txn);
            return container;
        }

        /// \brief Retrieves all pairs into the requested container.
        /// \tparam ContainerT Container type, defaulting to \c std::map.
        /// \param txn Active transaction wrapper.
        /// \return Filled container.
        template<template<class...> class ContainerT = std::map>
        typename detail::key_value_result_container<ContainerT, KeyT, ValueT>::type
        retrieve_all(const Transaction& txn) const {
            return retrieve_all<ContainerT>(txn.handle());
        }

        /// \brief Appends pairs to the store by upserting source keys.
        /// \tparam ContainerT Container type storing key-value pairs.
        /// \param container Source pairs.
        /// \param txn Optional transaction handle.
        template<template<class...> class ContainerT>
        void append(const ContainerT<KeyT, ValueT>& container, MDBX_txn* txn = nullptr) {
            with_transaction([this, &container](MDBX_txn* t) {
                db_append(container, t);
            }, TransactionMode::WRITABLE, txn);
        }

        /// \brief Appends pairs to the store by upserting source keys.
        /// \tparam ContainerT Container type storing key-value pairs.
        /// \param container Source pairs.
        /// \param txn Active transaction wrapper.
        template<template<class...> class ContainerT>
        void append(const ContainerT<KeyT, ValueT>& container, const Transaction& txn) {
            append(container, txn.handle());
        }

        /// \brief Appends vector pairs to the store by upserting source keys.
        /// \param container Source pairs.
        /// \param txn Optional transaction handle.
        void append(const std::vector<value_type>& container, MDBX_txn* txn = nullptr) {
            with_transaction([this, &container](MDBX_txn* t) {
                db_append(container, t);
            }, TransactionMode::WRITABLE, txn);
        }

        /// \brief Appends vector pairs to the store by upserting source keys.
        /// \param container Source pairs.
        /// \param txn Active transaction wrapper.
        void append(const std::vector<value_type>& container, const Transaction& txn) {
            append(container, txn.handle());
        }

        /// \brief Reconciles the store with pairs from a container.
        /// \tparam ContainerT Container type storing key-value pairs.
        /// \param container Source pairs.
        /// \param txn Optional transaction handle.
        template<template<class...> class ContainerT>
        void reconcile(const ContainerT<KeyT, ValueT>& container, MDBX_txn* txn = nullptr) {
            with_transaction([this, &container](MDBX_txn* t) {
                db_reconcile(container, t);
            }, TransactionMode::WRITABLE, txn);
        }

        /// \brief Reconciles the store with pairs from a container.
        /// \tparam ContainerT Container type storing key-value pairs.
        /// \param container Source pairs.
        /// \param txn Active transaction wrapper.
        template<template<class...> class ContainerT>
        void reconcile(const ContainerT<KeyT, ValueT>& container, const Transaction& txn) {
            reconcile(container, txn.handle());
        }

        /// \brief Reconciles the store with pairs from a vector.
        /// \param container Source pairs.
        /// \param txn Optional transaction handle.
        void reconcile(const std::vector<value_type>& container, MDBX_txn* txn = nullptr) {
            with_transaction([this, &container](MDBX_txn* t) {
                db_reconcile(container, t);
            }, TransactionMode::WRITABLE, txn);
        }

        /// \brief Reconciles the store with pairs from a vector.
        /// \param container Source pairs.
        /// \param txn Active transaction wrapper.
        void reconcile(const std::vector<value_type>& container, const Transaction& txn) {
            reconcile(container, txn.handle());
        }

        /// \brief Inserts a key-value pair only when the key is absent.
        /// \param key Key to insert.
        /// \param value Value to store.
        /// \param txn Optional transaction handle.
        /// \return \c true if inserted, \c false if the key already exists.
        bool insert(const KeyT& key, const ValueT& value, MDBX_txn* txn = nullptr) {
            bool res = false;
            with_transaction([this, &key, &value, &res](MDBX_txn* t) {
                res = db_insert_if_absent(key, value, t);
            }, TransactionMode::WRITABLE, txn);
            return res;
        }

        /// \brief Inserts a key-value pair only when the key is absent.
        /// \param key Key to insert.
        /// \param value Value to store.
        /// \param txn Active transaction wrapper.
        /// \return \c true if inserted, \c false if the key already exists.
        bool insert(const KeyT& key, const ValueT& value, const Transaction& txn) {
            return insert(key, value, txn.handle());
        }

        /// \brief Inserts a pair only when the key is absent.
        /// \param pair Pair to insert.
        /// \param txn Optional transaction handle.
        /// \return \c true if inserted, \c false if the key already exists.
        bool insert(const value_type& pair, MDBX_txn* txn = nullptr) {
            return insert(pair.first, pair.second, txn);
        }

        /// \brief Inserts a pair only when the key is absent.
        /// \param pair Pair to insert.
        /// \param txn Active transaction wrapper.
        /// \return \c true if inserted, \c false if the key already exists.
        bool insert(const value_type& pair, const Transaction& txn) {
            return insert(pair, txn.handle());
        }

        /// \brief Inserts or replaces the value for a key.
        /// \param key Key to upsert.
        /// \param value Value to store.
        /// \param txn Optional transaction handle.
        void insert_or_assign(const KeyT& key, const ValueT& value, MDBX_txn* txn = nullptr) {
            with_transaction([this, &key, &value](MDBX_txn* t) {
                db_insert_or_assign(key, value, t);
            }, TransactionMode::WRITABLE, txn);
        }

        /// \brief Inserts or replaces the value for a key.
        /// \param key Key to upsert.
        /// \param value Value to store.
        /// \param txn Active transaction wrapper.
        void insert_or_assign(const KeyT& key, const ValueT& value, const Transaction& txn) {
            insert_or_assign(key, value, txn.handle());
        }

        /// \brief Inserts or replaces a pair.
        /// \param pair Pair to upsert.
        /// \param txn Optional transaction handle.
        void insert_or_assign(const value_type& pair, MDBX_txn* txn = nullptr) {
            insert_or_assign(pair.first, pair.second, txn);
        }

        /// \brief Inserts or replaces a pair.
        /// \param pair Pair to upsert.
        /// \param txn Active transaction wrapper.
        void insert_or_assign(const value_type& pair, const Transaction& txn) {
            insert_or_assign(pair, txn.handle());
        }

        /// \brief Retrieves value by key or throws.
        /// \param key Key to look up.
        /// \param txn Optional transaction handle.
        /// \return Stored value.
        /// \throws std::out_of_range if the key is absent.
        ValueT at(const KeyT& key, MDBX_txn* txn = nullptr) const {
            ValueT value;
            with_transaction([this, &key, &value](MDBX_txn* t) {
                if (!db_get(key, value, t)) {
                    throw std::out_of_range("Key not found in hashed key-value store");
                }
            }, TransactionMode::READ_ONLY, txn);
            return value;
        }

        /// \brief Retrieves value by key or throws.
        /// \param key Key to look up.
        /// \param txn Active transaction wrapper.
        /// \return Stored value.
        ValueT at(const KeyT& key, const Transaction& txn) const {
            return at(key, txn.handle());
        }

        /// \brief Tries to retrieve a value by key.
        /// \param key Key to look up.
        /// \param out Output value when found.
        /// \param txn Optional transaction handle.
        /// \return \c true if found.
        bool try_get(const KeyT& key, ValueT& out, MDBX_txn* txn = nullptr) const {
            bool res = false;
            with_transaction([this, &key, &out, &res](MDBX_txn* t) {
                res = db_get(key, out, t);
            }, TransactionMode::READ_ONLY, txn);
            return res;
        }

        /// \brief Tries to retrieve a value by key.
        /// \param key Key to look up.
        /// \param out Output value when found.
        /// \param txn Active transaction wrapper.
        /// \return \c true if found.
        bool try_get(const KeyT& key, ValueT& out, const Transaction& txn) const {
            return try_get(key, out, txn.handle());
        }

#if __cplusplus >= 201703L
        /// \brief Finds a value by key.
        /// \param key Key to look up.
        /// \param txn Optional transaction handle.
        /// \return Optional stored value.
        std::optional<ValueT> find(const KeyT& key, MDBX_txn* txn = nullptr) const {
            std::optional<ValueT> result;
            with_transaction([this, &key, &result](MDBX_txn* t) {
                ValueT value;
                if (db_get(key, value, t)) {
                    result = std::move(value);
                }
            }, TransactionMode::READ_ONLY, txn);
            return result;
        }

        /// \brief Finds a value by key.
        /// \param key Key to look up.
        /// \param txn Active transaction wrapper.
        /// \return Optional stored value.
        std::optional<ValueT> find(const KeyT& key, const Transaction& txn) const {
            return find(key, txn.handle());
        }
#else
        /// \brief Finds a value by key.
        /// \param key Key to look up.
        /// \param txn Optional transaction handle.
        /// \return Pair of success flag and value.
        std::pair<bool, ValueT> find(const KeyT& key, MDBX_txn* txn = nullptr) const {
            return find_compat(key, txn);
        }

        /// \brief Finds a value by key.
        /// \param key Key to look up.
        /// \param txn Active transaction wrapper.
        /// \return Pair of success flag and value.
        std::pair<bool, ValueT> find(const KeyT& key, const Transaction& txn) const {
            return find_compat(key, txn.handle());
        }
#endif

        /// \brief Finds a value by key using a C++11-compatible result.
        /// \param key Key to look up.
        /// \param txn Optional transaction handle.
        /// \return Pair of success flag and value.
        std::pair<bool, ValueT> find_compat(const KeyT& key, MDBX_txn* txn = nullptr) const {
            std::pair<bool, ValueT> result(false, ValueT());
            with_transaction([this, &key, &result](MDBX_txn* t) {
                if (db_get(key, result.second, t)) {
                    result.first = true;
                }
            }, TransactionMode::READ_ONLY, txn);
            return result;
        }

        /// \brief Finds a value by key using a C++11-compatible result.
        /// \param key Key to look up.
        /// \param txn Active transaction wrapper.
        /// \return Pair of success flag and value.
        std::pair<bool, ValueT> find_compat(const KeyT& key, const Transaction& txn) const {
            return find_compat(key, txn.handle());
        }

        /// \brief Checks whether a key exists.
        /// \param key Key to look up.
        /// \param txn Optional transaction handle.
        /// \return \c true if present.
        bool contains(const KeyT& key, MDBX_txn* txn = nullptr) const {
            bool res = false;
            with_transaction([this, &key, &res](MDBX_txn* t) {
                res = db_contains(key, t);
            }, TransactionMode::READ_ONLY, txn);
            return res;
        }

        /// \brief Checks whether a key exists.
        /// \param key Key to look up.
        /// \param txn Active transaction wrapper.
        /// \return \c true if present.
        bool contains(const KeyT& key, const Transaction& txn) const {
            return contains(key, txn.handle());
        }

        /// \brief Counts stored key-value pairs.
        /// \param txn Optional transaction handle.
        /// \return Number of stored keys.
        std::size_t count(MDBX_txn* txn = nullptr) const {
            std::size_t res = 0;
            with_transaction([this, &res](MDBX_txn* t) {
                res = db_count(t);
            }, TransactionMode::READ_ONLY, txn);
            return res;
        }

        /// \brief Counts stored key-value pairs.
        /// \param txn Active transaction wrapper.
        /// \return Number of stored keys.
        std::size_t count(const Transaction& txn) const {
            return count(txn.handle());
        }

        /// \brief Checks whether the store has no records.
        /// \param txn Optional transaction handle.
        /// \return \c true if empty.
        bool empty(MDBX_txn* txn = nullptr) const {
            return count(txn) == 0;
        }

        /// \brief Checks whether the store has no records.
        /// \param txn Active transaction wrapper.
        /// \return \c true if empty.
        bool empty(const Transaction& txn) const {
            return empty(txn.handle());
        }

        /// \brief Erases a key.
        /// \param key Key to remove.
        /// \param txn Optional transaction handle.
        /// \return \c true if a record was removed.
        bool erase(const KeyT& key, MDBX_txn* txn = nullptr) {
            bool res = false;
            with_transaction([this, &key, &res](MDBX_txn* t) {
                res = db_erase(key, t);
            }, TransactionMode::WRITABLE, txn);
            return res;
        }

        /// \brief Erases a key.
        /// \param key Key to remove.
        /// \param txn Active transaction wrapper.
        /// \return \c true if a record was removed.
        bool erase(const KeyT& key, const Transaction& txn) {
            return erase(key, txn.handle());
        }

        /// \brief Removes all records.
        /// \param txn Optional transaction handle.
        void clear(MDBX_txn* txn = nullptr) {
            with_transaction([this](MDBX_txn* t) {
                db_clear(t);
            }, TransactionMode::WRITABLE, txn);
        }

        /// \brief Removes all records.
        /// \param txn Active transaction wrapper.
        void clear(const Transaction& txn) {
            clear(txn.handle());
        }

    private:
        Hasher m_hasher;

        struct PackedRecordView {
            const uint8_t* key_data;
            std::size_t key_size;
            const uint8_t* value_data;
            std::size_t value_size;
        };

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

        static MDBX_db_flags_t store_flags(MDBX_db_flags_t flags) noexcept {
            return flags | MDBX_INTEGERKEY | MDBX_DUPSORT;
        }

        static void write_u64_le(std::uint64_t value, uint8_t* out) noexcept {
            for (int i = 0; i < 8; ++i) {
                out[i] = static_cast<uint8_t>((value >> (8 * i)) & 0xffu);
            }
        }

        static std::uint64_t read_u64_le(const uint8_t* data) noexcept {
            std::uint64_t value = 0;
            for (int i = 0; i < 8; ++i) {
                value |= static_cast<std::uint64_t>(data[i]) << (8 * i);
            }
            return value;
        }

        static std::vector<uint8_t> copy_bytes(const void* data, std::size_t size) {
            std::vector<uint8_t> out(size);
            if (size) {
                std::memcpy(out.data(), data, size);
            }
            return out;
        }

        static std::vector<uint8_t> key_bytes(const KeyT& key) {
            ByteView view = make_byte_view(key);
            return copy_bytes(view.data, view.size);
        }

        std::uint64_t hash_key_bytes(const std::vector<uint8_t>& bytes) const {
            ByteView view(bytes.empty() ? nullptr : static_cast<const void*>(bytes.data()), bytes.size());
            return static_cast<std::uint64_t>(m_hasher(view));
        }

        MDBX_val hash_key_view(std::uint64_t hash, SerializeScratch& sc_hash) const {
            return serialize_key<true>(hash, sc_hash);
        }

        static PackedRecordView parse_record(const MDBX_val& db_val) {
            if (db_val.iov_len < 8 || !db_val.iov_base) {
                throw std::runtime_error("Corrupted hashed key-value record");
            }

            const uint8_t* data = static_cast<const uint8_t*>(db_val.iov_base);
            const std::uint64_t key_size64 = read_u64_le(data);
            const std::size_t available = db_val.iov_len - 8;
            if (key_size64 > static_cast<std::uint64_t>(available)) {
                throw std::runtime_error("Corrupted hashed key-value record");
            }

            PackedRecordView view;
            view.key_data = data + 8;
            view.key_size = static_cast<std::size_t>(key_size64);
            view.value_data = view.key_data + view.key_size;
            view.value_size = available - view.key_size;
            return view;
        }

        static bool key_matches(const PackedRecordView& record, const std::vector<uint8_t>& key) {
            if (record.key_size != key.size()) {
                return false;
            }
            if (key.empty()) {
                return true;
            }
            return std::memcmp(record.key_data, key.data(), key.size()) == 0;
        }

        template<class T>
        static typename std::enable_if<std::is_same<T, std::string>::value, T>::type
        make_key_from_bytes(const uint8_t* data, std::size_t size) {
            if (!size) {
                return T();
            }
            return T(reinterpret_cast<const char*>(size ? data : nullptr), size);
        }

        template<class T>
        static typename std::enable_if<!std::is_same<T, std::string>::value, T>::type
        make_key_from_bytes(const uint8_t* data, std::size_t size) {
            T out;
            out.resize(size);
            if (size) {
                std::memcpy(out.data(), data, size);
            }
            return out;
        }

        static KeyT deserialize_key_bytes(const uint8_t* data, std::size_t size) {
            return make_key_from_bytes<KeyT>(data, size);
        }

        static ValueT deserialize_payload_value(const PackedRecordView& record) {
            MDBX_val value_val = SerializeScratch::view(
                record.value_size ? record.value_data : nullptr,
                record.value_size
            );
            return deserialize_value<ValueT>(value_val);
        }

        static std::vector<uint8_t> make_record_payload(const std::vector<uint8_t>& key,
                                                        const ValueT& value) {
            SerializeScratch sc_value;
            MDBX_val raw_value = serialize_value(value, sc_value);
            std::vector<uint8_t> payload(8 + key.size() + raw_value.iov_len);
            write_u64_le(static_cast<std::uint64_t>(key.size()), payload.data());
            if (!key.empty()) {
                std::memcpy(payload.data() + 8, key.data(), key.size());
            }
            if (raw_value.iov_len) {
                std::memcpy(payload.data() + 8 + key.size(), raw_value.iov_base, raw_value.iov_len);
            }
            return payload;
        }

        void put_record(std::uint64_t hash,
                        const std::vector<uint8_t>& original_key,
                        const ValueT& value,
                        MDBX_txn* txn) {
            std::vector<uint8_t> payload = make_record_payload(original_key, value);
            SerializeScratch sc_hash;
            MDBX_val db_hash = hash_key_view(hash, sc_hash);
            MDBX_val db_val = SerializeScratch::view(payload.empty() ? nullptr : payload.data(), payload.size());
            int rc = mdbx_put(txn, m_dbi, &db_hash, &db_val, MDBX_NODUPDATA);
            if (rc == MDBX_KEYEXIST) {
                throw std::runtime_error("Hashed key-value duplicate record already exists");
            }
            check_mdbx(rc, "Failed to write hashed key-value record");
        }

        bool db_find_record_bytes(const std::vector<uint8_t>& key,
                                  std::uint64_t hash,
                                  MDBX_val* out_value,
                                  MDBX_txn* txn) const {
            MDBX_cursor* cursor = nullptr;
            check_mdbx(mdbx_cursor_open(txn, m_dbi, &cursor), "Failed to open hashed key-value cursor");
            try {
                SerializeScratch sc_hash;
                MDBX_val db_hash = hash_key_view(hash, sc_hash);
                MDBX_val db_val;
                int rc = mdbx_cursor_get(cursor, &db_hash, &db_val, MDBX_SET_KEY);
                if (rc == MDBX_NOTFOUND) {
                    mdbx_cursor_close(cursor);
                    return false;
                }
                check_mdbx(rc, "Failed to seek hashed key-value bucket");

                while (rc == MDBX_SUCCESS) {
                    PackedRecordView record = parse_record(db_val);
                    if (key_matches(record, key)) {
                        if (out_value) {
                            *out_value = db_val;
                        }
                        mdbx_cursor_close(cursor);
                        return true;
                    }
                    rc = mdbx_cursor_get(cursor, &db_hash, &db_val, MDBX_NEXT_DUP);
                }

                if (rc != MDBX_NOTFOUND) {
                    check_mdbx(rc, "Failed to scan hashed key-value bucket");
                }
                mdbx_cursor_close(cursor);
                return false;
            } catch (...) {
                mdbx_cursor_close(cursor);
                throw;
            }
        }

        bool delete_record_bytes(const std::vector<uint8_t>& key,
                                 std::uint64_t hash,
                                 MDBX_txn* txn) {
            MDBX_cursor* cursor = nullptr;
            check_mdbx(mdbx_cursor_open(txn, m_dbi, &cursor), "Failed to open hashed key-value cursor");
            try {
                SerializeScratch sc_hash;
                MDBX_val db_hash = hash_key_view(hash, sc_hash);
                MDBX_val db_val;
                int rc = mdbx_cursor_get(cursor, &db_hash, &db_val, MDBX_SET_KEY);
                if (rc == MDBX_NOTFOUND) {
                    mdbx_cursor_close(cursor);
                    return false;
                }
                check_mdbx(rc, "Failed to seek hashed key-value bucket");

                while (rc == MDBX_SUCCESS) {
                    PackedRecordView record = parse_record(db_val);
                    if (key_matches(record, key)) {
                        check_mdbx(mdbx_cursor_del(cursor, MDBX_CURRENT), "Failed to delete hashed key-value record");
                        mdbx_cursor_close(cursor);
                        return true;
                    }
                    rc = mdbx_cursor_get(cursor, &db_hash, &db_val, MDBX_NEXT_DUP);
                }

                if (rc != MDBX_NOTFOUND) {
                    check_mdbx(rc, "Failed to scan hashed key-value bucket");
                }
                mdbx_cursor_close(cursor);
                return false;
            } catch (...) {
                mdbx_cursor_close(cursor);
                throw;
            }
        }

        bool db_get(const KeyT& key, ValueT& value, MDBX_txn* txn) const {
            std::vector<uint8_t> original_key = key_bytes(key);
            const std::uint64_t hash = hash_key_bytes(original_key);
            MDBX_val db_val;
            if (!db_find_record_bytes(original_key, hash, &db_val, txn)) {
                return false;
            }
            value = deserialize_payload_value(parse_record(db_val));
            return true;
        }

        bool db_contains(const KeyT& key, MDBX_txn* txn) const {
            std::vector<uint8_t> original_key = key_bytes(key);
            const std::uint64_t hash = hash_key_bytes(original_key);
            return db_find_record_bytes(original_key, hash, nullptr, txn);
        }

        bool db_insert_if_absent(const KeyT& key, const ValueT& value, MDBX_txn* txn) {
            std::vector<uint8_t> original_key = key_bytes(key);
            const std::uint64_t hash = hash_key_bytes(original_key);
            if (db_find_record_bytes(original_key, hash, nullptr, txn)) {
                return false;
            }
            put_record(hash, original_key, value, txn);
            return true;
        }

        void db_insert_or_assign(const KeyT& key, const ValueT& value, MDBX_txn* txn) {
            std::vector<uint8_t> original_key = key_bytes(key);
            const std::uint64_t hash = hash_key_bytes(original_key);
            delete_record_bytes(original_key, hash, txn);
            put_record(hash, original_key, value, txn);
        }

        bool db_erase(const KeyT& key, MDBX_txn* txn) {
            std::vector<uint8_t> original_key = key_bytes(key);
            const std::uint64_t hash = hash_key_bytes(original_key);
            return delete_record_bytes(original_key, hash, txn);
        }

        std::size_t db_count(MDBX_txn* txn) const {
            std::size_t count = 0;
            MDBX_cursor* cursor = nullptr;
            check_mdbx(mdbx_cursor_open(txn, m_dbi, &cursor), "Failed to open hashed key-value cursor");
            try {
                MDBX_val db_hash, db_val;
                int rc = MDBX_SUCCESS;
                while ((rc = mdbx_cursor_get(cursor, &db_hash, &db_val, MDBX_NEXT)) == MDBX_SUCCESS) {
                    (void)db_hash;
                    (void)db_val;
                    ++count;
                }
                if (rc != MDBX_NOTFOUND) {
                    check_mdbx(rc, "Failed to count hashed key-value records");
                }
                mdbx_cursor_close(cursor);
                return count;
            } catch (...) {
                mdbx_cursor_close(cursor);
                throw;
            }
        }

        template<template<class...> class ContainerT>
        void db_load(ContainerT<KeyT, ValueT>& container, MDBX_txn* txn) const {
            MDBX_cursor* cursor = nullptr;
            check_mdbx(mdbx_cursor_open(txn, m_dbi, &cursor), "Failed to open hashed key-value cursor");
            try {
                MDBX_val db_hash, db_val;
                int rc = MDBX_SUCCESS;
                while ((rc = mdbx_cursor_get(cursor, &db_hash, &db_val, MDBX_NEXT)) == MDBX_SUCCESS) {
                    (void)db_hash;
                    PackedRecordView record = parse_record(db_val);
                    KeyT key = deserialize_key_bytes(record.key_data, record.key_size);
                    ValueT value = deserialize_payload_value(record);
                    container.emplace(std::move(key), std::move(value));
                }
                if (rc != MDBX_NOTFOUND) {
                    check_mdbx(rc, "Failed to read hashed key-value records");
                }
                mdbx_cursor_close(cursor);
            } catch (...) {
                mdbx_cursor_close(cursor);
                throw;
            }
        }

        void db_load(std::vector<value_type>& container, MDBX_txn* txn) const {
            MDBX_cursor* cursor = nullptr;
            check_mdbx(mdbx_cursor_open(txn, m_dbi, &cursor), "Failed to open hashed key-value cursor");
            try {
                MDBX_val db_hash, db_val;
                int rc = MDBX_SUCCESS;
                while ((rc = mdbx_cursor_get(cursor, &db_hash, &db_val, MDBX_NEXT)) == MDBX_SUCCESS) {
                    (void)db_hash;
                    PackedRecordView record = parse_record(db_val);
                    KeyT key = deserialize_key_bytes(record.key_data, record.key_size);
                    ValueT value = deserialize_payload_value(record);
                    container.emplace_back(std::move(key), std::move(value));
                }
                if (rc != MDBX_NOTFOUND) {
                    check_mdbx(rc, "Failed to read hashed key-value records");
                }
                mdbx_cursor_close(cursor);
            } catch (...) {
                mdbx_cursor_close(cursor);
                throw;
            }
        }

        template<template<class...> class ContainerT>
        void db_append(const ContainerT<KeyT, ValueT>& container, MDBX_txn* txn) {
            for (typename ContainerT<KeyT, ValueT>::const_iterator it = container.begin();
                 it != container.end(); ++it) {
                db_insert_or_assign(it->first, it->second, txn);
            }
        }

        void db_append(const std::vector<value_type>& container, MDBX_txn* txn) {
            for (typename std::vector<value_type>::const_iterator it = container.begin();
                 it != container.end(); ++it) {
                db_insert_or_assign(it->first, it->second, txn);
            }
        }

        template<class ContainerT>
        void db_reconcile_impl(const ContainerT& container, MDBX_txn* txn) {
            std::set<std::vector<uint8_t> > desired_keys;
            for (typename ContainerT::const_iterator it = container.begin(); it != container.end(); ++it) {
                std::vector<uint8_t> original_key = key_bytes(it->first);
                desired_keys.insert(original_key);
                const std::uint64_t hash = hash_key_bytes(original_key);
                delete_record_bytes(original_key, hash, txn);
                put_record(hash, original_key, it->second, txn);
            }

            MDBX_cursor* cursor = nullptr;
            check_mdbx(mdbx_cursor_open(txn, m_dbi, &cursor), "Failed to open hashed key-value cursor");
            try {
                MDBX_val db_hash, db_val;
                int rc = MDBX_SUCCESS;
                while ((rc = mdbx_cursor_get(cursor, &db_hash, &db_val, MDBX_NEXT)) == MDBX_SUCCESS) {
                    (void)db_hash;
                    PackedRecordView record = parse_record(db_val);
                    std::vector<uint8_t> stored_key = copy_bytes(record.key_data, record.key_size);
                    if (desired_keys.find(stored_key) == desired_keys.end()) {
                        check_mdbx(mdbx_cursor_del(cursor, MDBX_CURRENT), "Failed to delete stale hashed key-value record");
                    }
                }
                if (rc != MDBX_NOTFOUND) {
                    check_mdbx(rc, "Failed to scan hashed key-value records");
                }
                mdbx_cursor_close(cursor);
            } catch (...) {
                mdbx_cursor_close(cursor);
                throw;
            }
        }

        template<template<class...> class ContainerT>
        void db_reconcile(const ContainerT<KeyT, ValueT>& container, MDBX_txn* txn) {
            db_reconcile_impl(container, txn);
        }

        void db_reconcile(const std::vector<value_type>& container, MDBX_txn* txn) {
            db_reconcile_impl(container, txn);
        }

        void db_clear(MDBX_txn* txn) {
            check_mdbx(mdbx_drop(txn, m_dbi, 0), "Failed to clear hashed key-value records");
        }
    };

} // namespace mdbxc

#endif // _MDBX_CONTAINERS_HASHED_KEY_VALUE_STORE_HPP_INCLUDED
