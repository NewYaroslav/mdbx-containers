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

        /// \brief Loads all key-value pairs from the database into a container (e.g., std::map or std::unordered_map).
        /// \tparam ContainerT The type of the container (e.g., std::map or std::unordered_map).
        /// \return A container populated with all key-value pairs from the database.
        /// \throws MdbxException if a database error occurs.
        /// \note The transaction mode is taken from the database configuration.
        template<template <class...> class ContainerT = std::map>
        ContainerT<KeyT, ValueT> operator()() {
            ContainerT<KeyT, ValueT> container;
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
                ValueT def{};
                m_db.insert_or_assign(m_key, def);
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
        /// \throws MdbxException if a database error occurs.
        template<template <class...> class ContainerT>
        void load(ContainerT<KeyT, ValueT>& container, MDBX_txn* txn = nullptr) {
            with_transaction([this, &container](MDBX_txn* txn) {
                db_load(container, txn);
            }, TransactionMode::READ_ONLY, txn);
        }

        /// \brief Retrieves all key-value pairs.
        /// \tparam ContainerT Container type (e.g., std::map or std::unordered_map).
        /// \return A container with all key-value pairs.
        /// \throws MdbxException if a database error occurs.
        template<template <class...> class ContainerT>
        ContainerT<KeyT, ValueT> retrieve_all(MDBX_txn* txn = nullptr) {
            ContainerT<KeyT, ValueT> container;
            with_transaction([this, &container](MDBX_txn* txn) {
                db_load(container, txn);
            }, TransactionMode::READ_ONLY, txn);
            return container;
        }

        /// \brief Appends data to the database.
        /// \tparam ContainerT Container type (e.g., std::map or std::unordered_map).
        /// \param container Container with content to be synchronized.
        /// \throws MdbxException if a database error occurs.
        template<template <class...> class ContainerT>
        void append(const ContainerT<KeyT, ValueT>& container, MDBX_txn* txn = nullptr) {
            with_transaction([this, &container](MDBX_txn* txn) {
                db_append(container, txn);
            }, TransactionMode::WRITABLE, txn);
        }

        /// \brief Reconciles the database with the container.
        /// \tparam ContainerT Container type (e.g., std::map or std::unordered_map).
        /// \param container Container to be reconciled with the database.
        /// \throws MdbxException if a database error occurs.
        template<template <class...> class ContainerT>
        void reconcile(const ContainerT<KeyT, ValueT>& container, MDBX_txn* txn = nullptr) {
            with_transaction([this, &container](MDBX_txn* txn) {
                db_reconcile(container, txn);
            }, TransactionMode::WRITABLE, txn);
        }

        /// \brief Inserts key-value only if key is absent.
        /// \param key The key to be inserted.
        /// \param value The value to be inserted.
        /// \throws MdbxException if a database error occurs.
        void insert(const KeyT &key, const ValueT &value, MDBX_txn* txn = nullptr) {
            with_transaction([this, &key, &value](MDBX_txn* txn) {
                db_insert_if_absent(key, value, txn);
            }, TransactionMode::WRITABLE, txn);
        }

        /// \brief Inserts key-value only if key is absent.
        /// \param pair The key-value pair to be inserted.
        /// \throws MdbxException if a database error occurs.
        void insert(const std::pair<KeyT, ValueT> &pair, MDBX_txn* txn = nullptr) {
            with_transaction([this, &pair](MDBX_txn* txn) {
                db_insert_if_absent(pair.first, pair.second, txn);
            }, TransactionMode::WRITABLE, txn);
        }
        
        /// \brief Inserts or replaces key-value pair.
        /// \param key The key to be inserted.
        /// \param value The value to be inserted.
        /// \throws MdbxException if a database error occurs.
        void insert_or_assign(const KeyT &key, const ValueT &value, MDBX_txn* txn = nullptr) {
            with_transaction([this, &key, &value](MDBX_txn* txn) {
                db_insert_or_assign(key, value, txn);
            }, TransactionMode::WRITABLE, txn);
        }

        /// \brief Inserts or replaces key-value pair.
        /// \param pair The key-value pair to be inserted.
        /// \throws MdbxException if a database error occurs.
        void insert_or_assign(const std::pair<KeyT, ValueT> &pair, MDBX_txn* txn = nullptr) {
            insert_or_assign([&pair](MDBX_txn* txn) {
                db_insert_or_assign(pair.first, pair.second, txn);
            }, TransactionMode::WRITABLE, txn);
        }
        
        /// \brief Retrieves value by key or throws.
        /// \param key The key to look up.
        /// \param txn Active MDBX transaction.
        /// \return The value associated with the key.
        /// \throws std::out_of_range if key not found.
        /// \throws MdbxException if DB error occurs.
        ValueT at(const KeyT& key, MDBX_txn* txn) const {
            ValueT value;
            with_transaction([this, &key, &value](MDBX_txn* txn) {
                if (!db_get(key, value, txn)) {
                    throw std::out_of_range("Key not found in database");
                }
            }, TransactionMode::READ_ONLY, txn);
            return value;
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

        /// \brief Checks if the database is empty.
        /// \return True if the database is empty, false otherwise.
        /// \throws MdbxException if a database error occurs.
        bool empty(MDBX_txn* txn = nullptr) const {
            std::size_t res = 0;
            with_transaction([this, &res](MDBX_txn* txn) {
                res = db_count(txn);
            }, TransactionMode::READ_ONLY, txn);
            return (res == 0);
        }

        /// \brief Removes key from DB.
        /// \param key The key of the pair to be removed.
        /// \param txn Active transaction.
        /// \throws MdbxException if deletion fails.
        void erase(const KeyT &key, MDBX_txn* txn = nullptr) {
            with_transaction([this, &key](MDBX_txn* txn) {
                db_erase(key, txn);
            }, TransactionMode::WRITABLE, txn);
        }

        /// \brief Clears all key-value pairs from the database.
        /// \param txn Active transaction.
        /// \throws MdbxException if a database error occurs.
        void clear(MDBX_txn* txn = nullptr) {
            with_transaction([this](MDBX_txn* txn) {
                db_clear(txn);
            }, TransactionMode::WRITABLE, txn);
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
            txn = thread_txn();
            if (txn) {
                action(txn);
                return;
            }
            
            auto txn_guard = m_connection->transaction(mode);
            try {
                action(txn_guard.handle());
                txn_guard.commit();
            } catch(...) {
                try {
                    txn_guard.rollback();
                } catch(...) {}
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
            check_mdbx(mdbx_cursor_open(txn_handle, m_dbi, &cursor), "mdbx_cursor_open");

            MDBX_val db_key, db_val;
            while (mdbx_cursor_get(cursor, &db_key, &db_val, MDBX_NEXT) == MDBX_SUCCESS) {
                KeyT key = deserialize_value<KeyT>(db_key);
                ValueT value = deserialize_value<ValueT>(db_val);
                container.emplace(std::move(key), std::move(value));
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
            check_mdbx(rc, "mdbx_get");
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
            check_mdbx(rc, "mdbx_get (contains)");
            return true;
        }

        /// \brief Returns the number of elements in the database.
        /// \param txn_handle Active transaction handle.
        /// \return The number of key-value pairs in the database.
        /// \throws MdbxException if a database error occurs.
        std::size_t db_count(MDBX_txn* txn_handle) const {
            MDBX_stat stat;
#           if MDBX_VERSION_MAJOR > 0 || MDBX_VERSION_MINOR >= 14
            check_mdbx(mdbx_dbi_stat(txn_handle, m_dbi, &stat, sizeof(stat)), "mdbx_dbi_stat");
#           else
            check_mdbx(mdbx_dbi_stat(txn_handle, m_dbi, &stat), "mdbx_dbi_stat");
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
                    "mdbx_put"
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
            // 1. Собрать все ключи из контейнера
            std::unordered_set<KeyT> new_keys;
            for (const auto& pair : container) {
                new_keys.insert(pair.first);
                MDBX_val db_key = serialize_key(pair.first);
                MDBX_val db_val = serialize_value(pair.second);
                check_mdbx(
                    mdbx_put(txn_handle, m_dbi, &db_key, &db_val, MDBX_UPSERT),
                    "mdbx_put"
                );
            }

            // 2. Пройти по всем существующим ключам в БД и удалить лишние
            MDBX_cursor* cursor = nullptr;
            check_mdbx(mdbx_cursor_open(txn_handle, m_dbi, &cursor), "mdbx_cursor_open");

            MDBX_val db_key, db_val;
            while (mdbx_cursor_get(cursor, &db_key, &db_val, MDBX_NEXT) == MDBX_SUCCESS) {
                KeyT key = deserialize_value<KeyT>(db_key);
                if (!new_keys.contains(key)) {
                    check_mdbx(mdbx_cursor_del(cursor, 0), "mdbx_cursor_del");
                }
            }

            mdbx_cursor_close(cursor);
        }
        
        /// \brief Inserts a key-value pair only if the key does not already exist.
        /// \param key The key to insert.
        /// \param value The value to insert.
        /// \param txn_handle The active MDBX transaction.
        /// \throws MdbxException if the insert fails for reasons other than key existence.
        void db_insert_if_absent(const KeyT& key, const ValueT& value, MDBX_txn* txn_handle) {
            MDBX_val db_key = serialize_key(key);
            MDBX_val db_val = serialize_value(value);
            int rc = mdbx_put(txn_handle, m_dbi, &db_key, &db_val, MDBX_NOOVERWRITE);
            if (rc != MDBX_SUCCESS && rc != MDBX_KEYEXIST) {
                check_mdbx(rc, "mdbx_put (insert_if_absent)");
            }
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
                "mdbx_put (insert_or_assign)"
            );
        }

        /// \brief Removes a key from the database.
        /// \param key The key of the pair to be removed.
        /// \throws MdbxException if deletion fails.
        void db_erase(const KeyT& key, MDBX_txn* txn_handle) {
            MDBX_val db_key = serialize_key(key);
            check_mdbx(mdbx_del(txn_handle, m_dbi, &db_key, nullptr), "mdbx_del");
        }

        /// \brief Clears all key-value pairs from the database.
        /// \throws MdbxException if an MDBX error occurs.
        void db_clear(MDBX_txn* txn_handle) {
            check_mdbx(mdbx_drop(txn_handle, m_dbi, 0), "mdbx_drop");
        }

    }; // KeyValueTable

}; // namespace mdbxc

#endif // _MDBX_CONTAINERS_KEY_VALUE_TABLE_HPP_INCLUDED