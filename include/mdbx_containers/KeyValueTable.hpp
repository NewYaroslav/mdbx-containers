#pragma once
#ifndef _MDBX_CONTAINERS_KEY_VALUE_TABLE_HPP_INCLUDED
#define _MDBX_CONTAINERS_KEY_VALUE_TABLE_HPP_INCLUDED

/// \file KeyValueTable.hpp
/// \brief Declaration of the KeyValueTable class for managing key-value pairs in an MDBX database.

#include "common.hpp"
#include <map>
#include <unordered_set>

namespace mdbxc {

    /// \class KeyValueTable
    /// \ingroup mdbxc_tables
    /// \brief Template class for managing key-value pairs in an MDBX database.
    /// \tparam KeyT Type of the keys.
    /// \tparam ValueT Type of the values.
    /// \details This class provides functionality to store, retrieve, and manipulate key-value pairs in an MDBX database.
    /// It supports various container types, such as `std::map`, `std::unordered_map`, `std::vector`, and `std::list`.
    /// Key-value pairs can be inserted, reconciled, retrieved, and removed with transaction support. The class includes
    /// methods for bulk loading and appending data with transactional integrity, ensuring that operations are safely executed
    /// in a database environment. Additionally, temporary tables are used during reconciliation to ensure consistent data
    /// synchronization. This class also provides methods for checking the count and emptiness of the database, and efficiently
    /// handles database errors with detailed exception handling.
    template<class KeyT, class ValueT>
    class KeyValueTable final : public BaseTable {
    public:

        /// \brief Default constructor.
        KeyValueTable(std::shared_ptr<Connection> connection,
                      std::string name = "kv_store",
                      MDBX_db_flags_t flags = MDBX_DB_DEFAULTS | MDBX_CREATE) 
            : BaseTable(std::move(connection), std::move(name), flags | get_mdbx_flags<KeyT>())  {}

        /// \brief Constructor with configuration.
        /// \param config Configuration settings for the database.
        explicit KeyValueTable(const Config& config, 
                               std::string name = "kv_store",
                               MDBX_db_flags_t flags = MDBX_DB_DEFAULTS | MDBX_CREATE) 
            : BaseTable(Connection::create(config), std::move(name), flags | get_mdbx_flags<KeyT>()) {
        }

        /// \brief Destructor.
        ~KeyValueTable() override final = default;

        // --- Operators ---

        /// \brief Assigns a container (e.g., std::map or std::unordered_map) to the database.
        /// \param container The container with key-value pairs.
        /// \return Reference to this KeyValueTable.
        /// \throws MdbxException if a database error occurs.
        /// \note The transaction mode is taken from the database configuration.
        template<template <class...> class ContainerT>
        KeyValueTable& operator=(const ContainerT<KeyT, ValueT>& container) {
            with_transaction([this, &container](MDBX_txn* txn) {
                db_reconcile(container, txn);
            }, TransactionMode::WRITABLE);
            return *this;
        }
        
        /// \brief Assigns a vector of key-value pairs to the database.
        /// \param container The vector with key-value pairs.
        /// \return Reference to this KeyValueTable.
        /// \throws MdbxException if a database error occurs.
        /// \note The transaction mode is taken from the database configuration.
        KeyValueTable& operator=(const std::vector<std::pair<KeyT, ValueT>>& container) {
            with_transaction([this, &container](MDBX_txn* txn) {
                db_reconcile(container, txn);
            }, TransactionMode::WRITABLE);
            return *this;
        }

        /// \brief Loads all key-value pairs from the database into a container.
        /// \tparam ContainerT Container type (e.g., std::map, std::unordered_map, std::vector).
        /// \return Filled container.
        /// \throws MdbxException if a database error occurs.
        template<template<class...> class ContainerT = std::map>
        auto operator()() {
            using ReturnT = std::conditional_t<
                std::is_same_v<ContainerT<KeyT, ValueT>, std::vector<std::pair<KeyT, ValueT>>>,
                std::vector<std::pair<KeyT, ValueT>>,
                ContainerT<KeyT, ValueT>
            >;

            ReturnT container;
            with_transaction([this, &container](MDBX_txn* txn) {
                db_load(container, txn);
            }, TransactionMode::READ_ONLY);
            return container;
        }

        /// \brief Helper proxy for convenient assignment via operator[].
        class AssignmentProxy {
        public:
            /// \brief Constructs the proxy for a specific key.
            /// \param db Reference to the owning table.
            /// \param key Key associated with this proxy.
            AssignmentProxy(KeyValueTable& db, KeyT key)
                : m_db(db), m_key(std::move(key)) {}

            /// \brief Assigns a value to the stored key.
            /// \param value Value to store.
            /// \return Reference to this proxy.
            AssignmentProxy& operator=(const ValueT& value) {
                m_db.insert_or_assign(m_key, value);
                return *this;
            }

            /// \brief Implicit conversion to the value type.
            /// If the key does not exist, a default-constructed value is inserted.
            operator ValueT() const {
                auto val = m_db.find(m_key);
                if (val) return *val;
                ValueT def{}; // default construct if missing
                m_db.insert_or_assign(m_key, def); // persist the default value
                return def;
            }

        private:
            KeyValueTable& m_db; ///< Reference to the owning table.
            KeyT m_key;          ///< Key associated with this proxy.
        };
        
        /// \brief Provides convenient access to insert or read a value by key.
        /// \param key Key to access.
        /// \return Proxy object used for assignment or implicit read.
        AssignmentProxy operator[](const KeyT& key) {
            return AssignmentProxy(*this, key);
        }

        // --- Existing methods ---

        /// \brief Loads data from the database into the container.
        /// \tparam ContainerT Container type (e.g., std::map or std::unordered_map).
        /// \param container Container to be synchronized with database content.
        /// \param txn Active MDBX transaction.
        /// \throws MdbxException if a database error occurs.
        template<template <class...> class ContainerT>
        void load(ContainerT<KeyT, ValueT>& container, MDBX_txn* txn = nullptr) {
            with_transaction([this, &container](MDBX_txn* txn) {
                db_load(container, txn);
            }, TransactionMode::READ_ONLY, txn);
        }
        
        /// \brief Loads data from the database into the container using provided transaction.
        /// \tparam ContainerT Container type (e.g., std::map or std::unordered_map).
        /// \param container Container to be synchronized with database content.
        /// \param txn Transaction wrapper used for the operation.
        /// \throws MdbxException if a database error occurs.
        template<template <class...> class ContainerT>
        void load(ContainerT<KeyT, ValueT>& container, const Transaction& txn) {
            load(container, txn.handle());
        }
        
        /// \brief Loads all key-value pairs into a std::vector of pairs.
        /// \param container Container to be synchronized with database content.
        /// \param txn Transaction wrapper used for the operation.
        /// \throws MdbxException if a database error occurs.
        void load(std::vector<std::pair<KeyT, ValueT>>& container, MDBX_txn* txn = nullptr) {
            with_transaction([this, &container](MDBX_txn* txn) {
                db_load(container, txn);
            }, TransactionMode::READ_ONLY, txn);
        }
        
        /// \brief Loads all key-value pairs into a std::vector of pairs.
        /// \param container Container to be synchronized with database content.
        /// \param txn Transaction wrapper used for the operation.
        /// \throws MdbxException if a database error occurs.
        void load(std::vector<std::pair<KeyT, ValueT>>& container, const Transaction& txn) {
            load(container, txn.handle());
        }

        /// \brief Retrieves all key-value pairs into the specified container type.
        /// \tparam ContainerT Container type (e.g., std::map, std::unordered_map, std::vector).
        /// \param txn Optional transaction handle.
        /// \return Filled container.
        /// \throws MdbxException if a database error occurs.
        template<template<class...> class ContainerT = std::map>
        auto retrieve_all(MDBX_txn* txn = nullptr) {
            using ReturnT = std::conditional_t<
                std::is_same_v<ContainerT<KeyT, ValueT>, std::vector<std::pair<KeyT, ValueT>>>,
                std::vector<std::pair<KeyT, ValueT>>,
                ContainerT<KeyT, ValueT>
            >;

            ReturnT container;
            with_transaction([this, &container](MDBX_txn* txn) {
                db_load(container, txn);
            }, TransactionMode::READ_ONLY, txn);
            return container;
        }
        
        /// \brief Retrieves all key-value pairs into the specified container type.
        /// \tparam ContainerT Container type (e.g., std::map, std::unordered_map, std::vector).
        /// \param txn Transaction wrapper used for the retrieval.
        /// \return Filled container.
        /// \throws MdbxException if a database error occurs.
        template<template<class...> class ContainerT = std::map>
        auto retrieve_all(const Transaction& txn) {
            return retrieve_all<ContainerT>(txn.handle());
        }

        /// \brief Appends data to the database.
        /// \tparam ContainerT Container type (e.g., std::map or std::unordered_map).
        /// \param container Container with content to be synchronized.
        /// \param txn Active MDBX transaction.
        /// \throws MdbxException if a database error occurs.
        template<template <class...> class ContainerT>
        void append(const ContainerT<KeyT, ValueT>& container, MDBX_txn* txn = nullptr) {
            with_transaction([this, &container](MDBX_txn* txn) {
                db_append(container, txn);
            }, TransactionMode::WRITABLE, txn);
        }
        
        /// \brief Appends data to the database.
        /// \tparam ContainerT Container type (e.g., std::map or std::unordered_map).
        /// \param container Container with content to be synchronized.
        /// \param txn Optional transaction handle.
        /// \throws MdbxException if a database error occurs.
        template<template <class...> class ContainerT>
        void append(const ContainerT<KeyT, ValueT>& container, const Transaction& txn) {
            append(container, txn.handle());
        }
        
        
        /// \brief Appends data from a vector to the database.
        /// \param container Vector with content to be synchronized.
        /// \param txn Active MDBX transaction.
        /// \throws MdbxException if a database error occurs.
        void append(const std::vector<std::pair<KeyT, ValueT>>& container, MDBX_txn* txn = nullptr) {
            with_transaction([this, &container](MDBX_txn* txn) {
                db_append(container, txn);
            }, TransactionMode::WRITABLE, txn);
        }

        /// \brief Appends data from a vector to the database using a provided transaction.
        /// \param container Vector with content to be synchronized.
        /// \param txn Transaction wrapper.
        /// \throws MdbxException if a database error occurs.
        void append(const std::vector<std::pair<KeyT, ValueT>>& container, const Transaction& txn) {
            append(container, txn.handle());
        }

        /// \brief Reconciles the database with the container.
        /// \tparam ContainerT Container type (e.g., std::map or std::unordered_map).
        /// \param container Container to be reconciled with the database.
        /// \param txn Active MDBX transaction.
        /// \throws MdbxException if a database error occurs.
        template<template <class...> class ContainerT>
        void reconcile(const ContainerT<KeyT, ValueT>& container, MDBX_txn* txn = nullptr) {
            with_transaction([this, &container](MDBX_txn* txn) {
                db_reconcile(container, txn);
            }, TransactionMode::WRITABLE, txn);
        }
        
        /// \brief Reconciles the database with the container.
        /// \tparam ContainerT Container type (e.g., std::map or std::unordered_map).
        /// \param container Container to be reconciled with the database.
        /// \param txn Transaction wrapper used for the operation.
        /// \throws MdbxException if a database error occurs.
        template<template <class...> class ContainerT>
        void reconcile(const ContainerT<KeyT, ValueT>& container, const Transaction& txn) {
            reconcile(container, txn.handle());
        }
        
        /// \brief Reconciles the database with the vector of key-value pairs.
        /// \param container Vector to be reconciled with the database.
        /// \param txn Active MDBX transaction.
        /// \throws MdbxException if a database error occurs.
        void reconcile(const std::vector<std::pair<KeyT, ValueT>>& container, MDBX_txn* txn = nullptr) {
            with_transaction([this, &container](MDBX_txn* txn) {
                db_reconcile(container, txn);
            }, TransactionMode::WRITABLE, txn);
        }

        /// \brief Reconciles the database with the vector of key-value pairs using provided transaction.
        /// \param container Vector to be reconciled with the database.
        /// \param txn Transaction wrapper.
        /// \throws MdbxException if a database error occurs.
        void reconcile(const std::vector<std::pair<KeyT, ValueT>>& container, const Transaction& txn) {
            reconcile(container, txn.handle());
        }

        /// \brief Inserts key-value only if key is absent.
        /// \param key The key to be inserted.
        /// \param value The value to be inserted.
        /// \param txn Active MDBX transaction.
        /// \return
        /// \throws MdbxException if a database error occurs.
        bool insert(const KeyT &key, const ValueT &value, MDBX_txn* txn = nullptr) {
            bool res;
            with_transaction([this, &key, &value, &res](MDBX_txn* txn) {
                res = db_insert_if_absent(key, value, txn);
            }, TransactionMode::WRITABLE, txn);
            return res;
        }
        
        /// \brief Inserts key-value only if key is absent.
        /// \param key The key to be inserted.
        /// \param value The value to be inserted.
        /// \param txn Transaction wrapper used for the insertion.
        /// \return
        /// \throws MdbxException if a database error occurs.
        bool insert(const KeyT &key, const ValueT &value, const Transaction& txn) {
            return insert(key, value, txn.handle());
        }

        /// \brief Inserts key-value only if key is absent.
        /// \param pair The key-value pair to be inserted.
        /// \param txn Active MDBX transaction.
        /// \return
        /// \throws MdbxException if a database error occurs.
        bool insert(const std::pair<KeyT, ValueT> &pair, MDBX_txn* txn = nullptr) {
            bool res;
            with_transaction([this, &pair, &res](MDBX_txn* txn) {
                res = db_insert_if_absent(pair.first, pair.second, txn);
            }, TransactionMode::WRITABLE, txn);
            return res;
        }
        
        /// \brief Inserts key-value only if key is absent.
        /// \param pair The key-value pair to be inserted.
        /// \param txn Transaction wrapper used for the operation.
        /// \return
        /// \throws MdbxException if a database error occurs.
        bool insert(const std::pair<KeyT, ValueT> &pair, const Transaction& txn) {
            return insert(pair, txn.handle());
        }
        
        /// \brief Inserts or replaces key-value pair.
        /// \param key The key to be inserted.
        /// \param value The value to be inserted.
        /// \param txn Active MDBX transaction.
        /// \throws MdbxException if a database error occurs.
        void insert_or_assign(const KeyT &key, const ValueT &value, MDBX_txn* txn = nullptr) {
            with_transaction([this, &key, &value](MDBX_txn* txn) {
                db_insert_or_assign(key, value, txn);
            }, TransactionMode::WRITABLE, txn);
        }
        
        /// \brief Inserts or replaces key-value pair.
        /// \param key The key to be inserted.
        /// \param value The value to be inserted.
        /// \param txn Transaction wrapper used for the operation.
        /// \throws MdbxException if a database error occurs.
        void insert_or_assign(const KeyT &key, const ValueT &value, const Transaction& txn) {
            insert_or_assign(key, value, txn.handle());
        }

        /// \brief Inserts or replaces key-value pair.
        /// \param pair The key-value pair to be inserted.
        /// \param txn Active MDBX transaction.
        /// \throws MdbxException if a database error occurs.
        void insert_or_assign(const std::pair<KeyT, ValueT> &pair, MDBX_txn* txn = nullptr) {
            insert_or_assign([&pair](MDBX_txn* txn) {
                db_insert_or_assign(pair.first, pair.second, txn);
            }, TransactionMode::WRITABLE, txn);
        }
        
        /// \brief Inserts or replaces key-value pair.
        /// \param pair The key-value pair to be inserted.
        /// \param txn Transaction wrapper used for the operation.
        /// \throws MdbxException if a database error occurs.
        void insert_or_assign(const std::pair<KeyT, ValueT> &pair, const Transaction& txn) {
            insert_or_assign(pair, txn.handle());
        }
        
        /// \brief Retrieves value by key or throws.
        /// \param key The key to look up.
        /// \param txn Active MDBX transaction.
        /// \return The value associated with the key.
        /// \throws std::out_of_range if key not found.
        /// \throws MdbxException if DB error occurs.
        ValueT at(const KeyT& key, MDBX_txn* txn = nullptr) const {
            ValueT value;
            with_transaction([this, &key, &value](MDBX_txn* txn) {
                if (!db_get(key, value, txn)) {
                    throw std::out_of_range("Key not found in database");
                }
            }, TransactionMode::READ_ONLY, txn);
            return value;
        }
        
        /// \brief Retrieves value by key or throws.
        /// \param key The key to look up.
        /// \param txn Transaction wrapper used for the lookup.
        /// \return The value associated with the key.
        /// \throws std::out_of_range if key not found.
        /// \throws MdbxException if DB error occurs.
        ValueT at(const KeyT& key, const Transaction& txn) const {
            return at(key, txn.handle());
        }
        
        /// \brief Tries to find value by key.
        /// \param key The key to look up.
        /// \param out Reference to output value.
        /// \param txn Active MDBX transaction.
        /// \return True if key exists, false otherwise.
        /// \throws MdbxException if DB error occurs.
        bool try_get(const KeyT& key, ValueT& out, MDBX_txn* txn) const {
            bool res;
            with_transaction([this, &key, &out, &res](MDBX_txn* txn) {
                res = db_get(key, out, txn);
            }, TransactionMode::READ_ONLY, txn);
            return res;
        }
        
        /// \brief Tries to find value by key.
        /// \param key The key to look up.
        /// \param out Reference to output value.
        /// \param txn Transaction wrapper used for the lookup.
        /// \return True if key exists, false otherwise.
        /// \throws MdbxException if DB error occurs.
        bool try_get(const KeyT& key, ValueT& out, const Transaction& txn) const {
            return try_get(key, out, txn.handle());
        }

#if __cplusplus >= 201703L   
        /// \brief Finds value by key.
        /// \param key Key to search for.
        /// \param txn Optional active MDBX transaction.
        /// \return std::optional with value if found, std::nullopt otherwise.
        /// \throws MdbxException on DB error.
        std::optional<ValueT> find(const KeyT& key, MDBX_txn* txn = nullptr) const {
            std::optional<ValueT> result;
            with_transaction([this, &key, &result](MDBX_txn* txn) {
                ValueT tmp;
                if (db_get(key, tmp, txn)) {
                    result = std::move(tmp);
                }
            }, TransactionMode::READ_ONLY, txn);
            return result;
        }
        
        /// \brief Finds value by key.
        /// \param key Key to search for.
        /// \param txn Transaction wrapper used for the lookup.
        /// \return std::optional with value if found, std::nullopt otherwise.
        /// \throws MdbxException on DB error.
        std::optional<ValueT> find(const KeyT& key, const Transaction& txn) const {
            return find(key, txn.handle());
        }
#endif

        /// \brief Finds value by key.
        /// \param key Key to search for.
        /// \param txn Optional active MDBX transaction.
        /// \return Pair of success flag and value.
        /// \throws MdbxException on DB error.
        std::pair<bool, ValueT> find_compat(const KeyT& key, MDBX_txn* txn = nullptr) const {
            std::pair<bool, ValueT> result{false, ValueT{}};
            with_transaction([this, &key, &result](MDBX_txn* txn) {
                if (db_get(key, result.second, txn)) {
                    result.first = true;
                }
            }, TransactionMode::READ_ONLY, txn);
            return result;
        }
        
        /// \brief Finds value by key.
        /// \param key Key to search for.
        /// \param txn Transaction wrapper used for the lookup.
        /// \return Pair of success flag and value.
        /// \throws MdbxException on DB error.
        std::pair<bool, ValueT> find_compat(const KeyT& key, const Transaction& txn) const {
            return find_compat(key, txn.handle());
        }

        /// \brief Checks whether a key exists in the database.
        /// \param key The key to look up.
        /// \param txn Active transaction.
        /// \return True if the key exists, false otherwise.
        /// \throws MdbxException if DB error occurs.
        bool contains(const KeyT& key, MDBX_txn* txn = nullptr) const {
            bool res;
            with_transaction([this, &key, &res](MDBX_txn* txn) {
                res = db_contains(key, txn);
            }, TransactionMode::READ_ONLY, txn);
            return res;
        }
        
        /// \brief Checks whether a key exists in the database.
        /// \param key The key to look up.
        /// \param txn Transaction wrapper used for the lookup.
        /// \return True if the key exists, false otherwise.
        /// \throws MdbxException if DB error occurs.
        bool contains(const KeyT& key, const Transaction& txn) const {
            return contains(key, txn.handle());
        }

        /// \brief Returns the number of elements in the database.
        /// \param txn Active transaction.
        /// \return The number of key-value pairs.
        /// \throws MdbxException if DB error occurs.
        std::size_t count(MDBX_txn* txn = nullptr) const {
            std::size_t res = 0;
            with_transaction([this, &res](MDBX_txn* txn) {
                res = db_count(txn);
            }, TransactionMode::READ_ONLY, txn);
            return res;
        }
        
        /// \brief Returns the number of elements in the database.
        /// \param txn Transaction wrapper used for the count operation.
        /// \return The number of key-value pairs.
        /// \throws MdbxException if DB error occurs.
        std::size_t count(const Transaction& txn) const {
            return count(txn.handle());
        }

        /// \brief Checks if the database is empty.
        /// \param txn Optional transaction handle.
        /// \return True if the database is empty, false otherwise.
        /// \throws MdbxException if a database error occurs.
        bool empty(MDBX_txn* txn = nullptr) const {
            std::size_t res = 0;
            with_transaction([this, &res](MDBX_txn* txn) {
                res = db_count(txn);
            }, TransactionMode::READ_ONLY, txn);
            return (res == 0);
        }
        
        /// \brief Checks if the database is empty.
        /// \param txn Transaction wrapper used for the check.
        /// \return True if the database is empty, false otherwise.
        /// \throws MdbxException if a database error occurs.
        bool empty(const Transaction& txn) const {
            return empty(txn.handle());
        }

        /// \brief Removes key from DB.
        /// \param key The key of the pair to be removed.
        /// \param txn Active transaction.
        /// \return True if the key was found and deleted, false if the key was not found.
        /// \throws MdbxException if deletion fails.
        bool erase(const KeyT &key, MDBX_txn* txn = nullptr) {
            bool res;
            with_transaction([this, &key, &res](MDBX_txn* txn) {
                res = db_erase(key, txn);
            }, TransactionMode::WRITABLE, txn);
            return res;
        }
        
        /// \brief Removes key from DB.
        /// \param key The key of the pair to be removed.
        /// \param txn Transaction wrapper used for the operation.
        /// \return True if the key was found and deleted, false if the key was not found.
        /// \throws MdbxException if deletion fails.
        bool erase(const KeyT &key, const Transaction& txn) {
            return erase(key, txn.handle());
        }

        /// \brief Clears all key-value pairs from the database.
        /// \param txn Active transaction.
        /// \throws MdbxException if a database error occurs.
        void clear(MDBX_txn* txn = nullptr) {
            with_transaction([this](MDBX_txn* txn) {
                db_clear(txn);
            }, TransactionMode::WRITABLE, txn);
        }
        
        /// \brief Clears all key-value pairs from the database.
        /// \param txn Transaction wrapper used for the operation.
        /// \throws MdbxException if a database error occurs.
        void clear(const Transaction& txn) {
            clear(txn.handle());
        }

    private:
    
        /// \brief Executes a functor within a transaction context.
        /// \tparam F Callable type accepting `MDBX_txn*`.
        /// \param action Functor to execute.
        /// \param mode Transaction mode used when a new transaction is created.
        /// \param txn Optional existing transaction handle.
        template<typename F>
        void with_transaction(F&& action, TransactionMode mode = TransactionMode::WRITABLE, MDBX_txn* txn = nullptr) const {
            if (txn) {
                action(txn);
                return;
            }
            txn = thread_txn(); // reuse transaction bound to this thread if any
            if (txn) {
                action(txn);
                return;
            }
            
            auto txn_guard = m_connection->transaction(mode);
            try {
                action(txn_guard.handle());
                txn_guard.commit();
            } catch(...) {
                // Ensure rollback on any exception and rethrow to the caller
                try {
                    txn_guard.rollback();
                } catch(...) {
                    // Ignore rollback errors here
                }
                throw;
            }
        }
    
        /// \brief Loads data from the database into the container.
        /// \tparam ContainerT Template for the container type.
        /// \param container Container to be synchronized with database content.
        /// \param txn_handle Active transaction handle.
        /// \throws MdbxException if a database error occurs.
        template<template <class...> class ContainerT>
        void db_load(ContainerT<KeyT, ValueT>& container, MDBX_txn* txn_handle) {
            MDBX_cursor* cursor = nullptr;
            check_mdbx(mdbx_cursor_open(txn_handle, m_dbi, &cursor), "Failed to open MDBX cursor");

            MDBX_val db_key, db_val;
            while (mdbx_cursor_get(cursor, &db_key, &db_val, MDBX_NEXT) == MDBX_SUCCESS) {
                auto&& key = deserialize_value<KeyT>(db_key);
                auto&& value = deserialize_value<ValueT>(db_val);
                container.emplace(std::move(key), std::move(value));
            }

            mdbx_cursor_close(cursor);
        }
        
        /// \brief Loads all key-value pairs into a std::vector of pairs.
        /// \param out_vector The output vector to fill.
        /// \param txn Optional transaction.
        /// \throws MdbxException if a database error occurs.
        void db_load(std::vector<std::pair<KeyT, ValueT>>& out_vector, MDBX_txn* txn) {
            MDBX_cursor* cursor = nullptr;
            check_mdbx(mdbx_cursor_open(txn, m_dbi, &cursor), "Failed to open MDBX cursor");

            MDBX_val db_key, db_val;
            while (mdbx_cursor_get(cursor, &db_key, &db_val, MDBX_NEXT) == MDBX_SUCCESS) {
                auto&& key = deserialize_value<KeyT>(db_key);
                auto&& value = deserialize_value<ValueT>(db_val);
                out_vector.emplace_back(std::move(key), std::move(value));
            }

            mdbx_cursor_close(cursor);
        }

        /// \brief Gets a value by key from the database.
        /// \param key The key to search for.
        /// \param value Reference to store the retrieved value.
        /// \param txn_handle The active transaction.
        /// \return True if found, false otherwise.
        /// \throws MdbxException if a database error occurs.
        bool db_get(const KeyT& key, ValueT& value, MDBX_txn* txn_handle) const {
            MDBX_val db_key = serialize_key(key);
            MDBX_val db_val;
            int rc = mdbx_get(txn_handle, m_dbi, &db_key, &db_val);
            if (rc == MDBX_NOTFOUND) return false;
            check_mdbx(rc, "Failed to retrieve value");
            value = deserialize_value<ValueT>(db_val);
            return true;
        }

        /// \brief Checks whether a key exists in the database.
        /// \param key The key to check.
        /// \param txn_handle The active transaction.
        /// \return True if key exists, false otherwise.
        /// \throws MdbxException if a database error occurs.
        bool db_contains(const KeyT& key, MDBX_txn* txn_handle) const {
            MDBX_val db_key = serialize_key(key);
            MDBX_val db_val; // dummy
            int rc = mdbx_get(txn_handle, m_dbi, &db_key, &db_val);
            if (rc == MDBX_NOTFOUND) return false;
            check_mdbx(rc, "Failed to check key presence");
            return true;
        }

        /// \brief Returns the number of elements in the database.
        /// \param txn_handle Active transaction handle.
        /// \return The number of key-value pairs in the database.
        /// \throws MdbxException if a database error occurs.
        std::size_t db_count(MDBX_txn* txn_handle) const {
            MDBX_stat stat;
#           if MDBX_VERSION_MAJOR > 0 || MDBX_VERSION_MINOR >= 14
            check_mdbx(mdbx_dbi_stat(txn_handle, m_dbi, &stat, sizeof(stat)), "Failed to query database statistics");
#           else
            check_mdbx(mdbx_dbi_stat(txn_handle, m_dbi, &stat), "Failed to query database statistics");
#           endif
            return stat.ms_entries;
        }

        /// \brief Appends the content of the container to the database.
        /// \tparam ContainerT Template for the container type (map or unordered_map).
        /// \param container Container with content to append.
        /// \param txn_handle Active transaction handle.
        /// \throws MdbxException if a database error occurs.
        template<template <class...> class ContainerT>
        void db_append(const ContainerT<KeyT, ValueT>& container, MDBX_txn* txn_handle) {
            for (const auto& pair : container) {
                MDBX_val db_key = serialize_key(pair.first);
                MDBX_val db_val = serialize_value(pair.second);
                check_mdbx(
                    mdbx_put(txn_handle, m_dbi, &db_key, &db_val, MDBX_UPSERT),
                    "Failed to write record"
                );
            }
        }
        
        /// \brief Appends the content of the vector to the database.
        /// \param container Vector with key-value pairs to append.
        /// \param txn_handle Active transaction handle.
        /// \throws MdbxException if a database error occurs.
        void db_append(const std::vector<std::pair<KeyT, ValueT>>& container, MDBX_txn* txn_handle) {
            for (const auto& [key, value] : container) {
                MDBX_val db_key = serialize_key(key);
                MDBX_val db_val = serialize_value(value);
                check_mdbx(
                    mdbx_put(txn_handle, m_dbi, &db_key, &db_val, MDBX_UPSERT),
                    "Failed to write record"
                );
            }
        }

        /// \brief Reconciles the content of the database with the container.
        /// Synchronizes the main table with the content of the container by using a temporary table.
        /// Clears old data, inserts new data, and updates existing records in the main table.
        /// \tparam ContainerT Template for the container type (map or unordered_map).
        /// \param container Container with key-value pairs to be reconciled with the database.
        /// \throws MdbxException if a database error occurs.
        template<template <class...> class ContainerT>
        void db_reconcile(const ContainerT<KeyT, ValueT>& container, MDBX_txn* txn_handle) {
            // 1. Collect all keys from the container
            std::unordered_set<KeyT> new_keys;
            for (const auto& pair : container) {
                new_keys.insert(pair.first);
                MDBX_val db_key = serialize_key(pair.first);
                MDBX_val db_val = serialize_value(pair.second);
                check_mdbx(
                    mdbx_put(txn_handle, m_dbi, &db_key, &db_val, MDBX_UPSERT),
                    "Failed to write record"
                );
            }

            // 2. Iterate over existing keys in the DB and remove the extras
            MDBX_cursor* cursor = nullptr;
            check_mdbx(mdbx_cursor_open(txn_handle, m_dbi, &cursor), "Failed to open MDBX cursor");

            MDBX_val db_key, db_val;
            while (mdbx_cursor_get(cursor, &db_key, &db_val, MDBX_NEXT) == MDBX_SUCCESS) {
                auto&& key = deserialize_value<KeyT>(db_key);
                if (new_keys.find(key) == new_keys.end()) {
                    check_mdbx(mdbx_cursor_del(cursor, MDBX_CURRENT), "Failed to delete record using cursor");
                }
            }

            mdbx_cursor_close(cursor);
        }
        
        /// \brief Reconciles the content of the database with the given vector of key-value pairs.
        /// Clears existing records not present in the vector, and upserts all from the vector.
        /// \param container Vector of key-value pairs to reconcile with the database.
        /// \param txn_handle Active MDBX transaction.
        /// \throws MdbxException if a database error occurs.
        void db_reconcile(const std::vector<std::pair<KeyT, ValueT>>& container, MDBX_txn* txn_handle) {
            // 1. Collect keys and upsert values
            std::unordered_set<KeyT> new_keys;
            for (size_t i = 0; i < container.size(); ++i) {
                const KeyT& key = container[i].first;
                const ValueT& value = container[i].second;

                new_keys.insert(key);

                MDBX_val db_key = serialize_key(key);
                MDBX_val db_val = serialize_value(value);
                check_mdbx(
                    mdbx_put(txn_handle, m_dbi, &db_key, &db_val, MDBX_UPSERT),
                    "Failed to write record"
                );
            }

            // 2. Delete stale keys from DB
            MDBX_cursor* cursor = nullptr;
            check_mdbx(mdbx_cursor_open(txn_handle, m_dbi, &cursor), "Failed to open MDBX cursor");

            MDBX_val db_key, db_val;
            while (mdbx_cursor_get(cursor, &db_key, &db_val, MDBX_NEXT) == MDBX_SUCCESS) {
                KeyT key = deserialize_value<KeyT>(db_key);
                if (new_keys.find(key) == new_keys.end()) {
                    check_mdbx(mdbx_cursor_del(cursor, MDBX_CURRENT), "Failed to delete record using cursor");
                }
            }

            mdbx_cursor_close(cursor);
        }
        
        /// \brief Inserts a key-value pair only if the key does not already exist.
        /// \param key The key to insert.
        /// \param value The value to insert.
        /// \param txn_handle The active MDBX transaction.
        /// \return true if the key-value pair was inserted, false if the key already existed.
        /// \throws MdbxException if the insert fails for reasons other than key existence.
        bool db_insert_if_absent(const KeyT& key, const ValueT& value, MDBX_txn* txn_handle) {
            MDBX_val db_key = serialize_key(key);
            MDBX_val db_val = serialize_value(value);
            int rc = mdbx_put(txn_handle, m_dbi, &db_key, &db_val, MDBX_NOOVERWRITE);

            if (rc == MDBX_SUCCESS)
                return true;
            if (rc == MDBX_KEYEXIST)
                return false;

            check_mdbx(rc, "Failed to insert key-value pair");
            return false;
        }
        
        /// \brief Inserts or replaces the key-value pair.
        /// \param key The key to insert or replace.
        /// \param value The value to set.
        /// \param txn_handle The active MDBX transaction.
        /// \throws MdbxException if the operation fails.
        void db_insert_or_assign(const KeyT& key, const ValueT& value, MDBX_txn* txn_handle) {
            MDBX_val db_key = serialize_key(key);
            MDBX_val db_val = serialize_value(value);
            check_mdbx(
                mdbx_put(txn_handle, m_dbi, &db_key, &db_val, MDBX_UPSERT),  // or 0
                "Failed to insert or assign key-value pair"
            );
        }

        /// \brief Removes a key from the database.
        /// \param key The key of the pair to be removed.
        /// \param txn_handle The active MDBX transaction.
        /// \return True if the key was found and deleted, false if the key was not found.
        /// \throws MdbxException if deletion fails for other reasons.
        bool db_erase(const KeyT& key, MDBX_txn* txn_handle) {
            MDBX_val db_key = serialize_key(key);
            int rc = mdbx_del(txn_handle, m_dbi, &db_key, nullptr);
            if (rc == MDBX_SUCCESS) return true;
            if (rc == MDBX_NOTFOUND) return false;
            check_mdbx(rc, "Failed to erase key");
            return false;
        }

        /// \brief Clears all key-value pairs from the database.
        /// \throws MdbxException if an MDBX error occurs.
        void db_clear(MDBX_txn* txn_handle) {
            check_mdbx(mdbx_drop(txn_handle, m_dbi, 0), "Failed to clear table");
        }

    }; // KeyValueTable

}; // namespace mdbxc

#endif // _MDBX_CONTAINERS_KEY_VALUE_TABLE_HPP_INCLUDED