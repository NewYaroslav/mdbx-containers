#pragma once
#ifndef _MDBX_CONTAINERS_KEY_TABLE_HPP_INCLUDED
#define _MDBX_CONTAINERS_KEY_TABLE_HPP_INCLUDED

/// \file KeyTable.hpp
/// \brief Set-like table storing keys without user values.
/// \details
/// Persists unique serialized keys with empty MDBX values, providing a compact
/// \c std::set -like workflow over an MDBX table.

#include "common.hpp"

namespace mdbxc {

    /// \class KeyTable
    /// \ingroup mdbxc_tables
    /// \brief Ordered key-only table persisted in MDBX.
    /// \tparam KeyT Type of the keys.
    /// \tparam Options Compile-time table policy. Does not change the database
    ///         storage format.
    /// \details
    /// Provides persistent \c std::set -like semantics: every key is stored at
    /// most once, duplicate inserts are ignored, and reads return keys ordered by
    /// the underlying MDBX key comparator. Stored records use the serialized key
    /// as the MDBX key and an empty MDBX value.
    ///
    /// \note \c append() inserts missing keys and leaves existing keys
    ///       unchanged. \c reconcile() currently clears the table and appends the
    ///       source keys, so it should be treated as replacement rather than an
    ///       incremental set diff.
    template<class KeyT, class Options = DefaultTableOptions>
    class KeyTable final : public BaseTable {
    public:
        /// \brief Constructs table using existing connection.
        /// \param connection Existing connection.
        /// \param name Name of the table within the MDBX environment.
        /// \param flags Additional MDBX database flags.
        KeyTable(std::shared_ptr<Connection> connection,
                 std::string name = "key_store",
                 MDBX_db_flags_t flags = MDBX_DB_DEFAULTS | MDBX_CREATE)
            : BaseTable(std::move(connection),
                        std::move(name),
                        flags | get_mdbx_flags<KeyT>()) {}

        /// \brief Constructs table using configuration.
        /// \param config Configuration settings.
        /// \param name Name of the table within the MDBX environment.
        /// \param flags Additional MDBX database flags.
        explicit KeyTable(const Config& config,
                          std::string name = "key_store",
                          MDBX_db_flags_t flags = MDBX_DB_DEFAULTS | MDBX_CREATE)
            : BaseTable(Connection::create(config),
                        std::move(name),
                        flags | get_mdbx_flags<KeyT>()) {}

        /// \brief Destructor.
        ~KeyTable() override = default;

        /// \brief Replaces table content with keys from a container.
        /// \tparam ContainerT Container type storing keys.
        /// \param container Source keys.
        /// \return Reference to this table.
        /// \note Equivalent to \c reconcile(container): current table content is
        ///       replaced by the source key set.
        template<template<class...> class ContainerT>
        KeyTable& operator=(const ContainerT<KeyT>& container) {
            reconcile(container);
            return *this;
        }

        /// \brief Loads all keys into a container.
        /// \tparam ContainerT Container type storing keys.
        /// \return Filled container.
        /// \note The default \c std::set preserves unique-key table semantics.
        ///       Other containers receive keys according to their insertion
        ///       rules.
        template<template<class...> class ContainerT = std::set>
        ContainerT<KeyT> operator()() const {
            return retrieve_all<ContainerT>();
        }

        /// \brief Loads keys from the database into a container.
        /// \tparam ContainerT Container type storing keys.
        /// \param container Output container.
        /// \param txn Optional transaction handle.
        /// \note Appends/inserts into \p container; it does not clear existing
        ///       container contents first.
        template<template<class...> class ContainerT>
        void load(ContainerT<KeyT>& container, MDBX_txn* txn = nullptr) const {
            with_transaction([this, &container](MDBX_txn* t) {
                db_load(container, t);
            }, TransactionMode::READ_ONLY, txn);
        }

        /// \brief Loads keys from the database into a container.
        /// \tparam ContainerT Container type storing keys.
        /// \param container Output container.
        /// \param txn Active transaction wrapper.
        template<template<class...> class ContainerT>
        void load(ContainerT<KeyT>& container, const Transaction& txn) const {
            load(container, txn.handle());
        }

        /// \brief Retrieves all keys into the requested container.
        /// \tparam ContainerT Container type storing keys.
        /// \param txn Optional transaction handle.
        /// \return Filled container.
        /// \note Returns a fresh container populated from the table.
        template<template<class...> class ContainerT = std::set>
        ContainerT<KeyT> retrieve_all(MDBX_txn* txn = nullptr) const {
            ContainerT<KeyT> container;
            load(container, txn);
            return container;
        }

        /// \brief Retrieves all keys into the requested container.
        /// \tparam ContainerT Container type storing keys.
        /// \param txn Active transaction wrapper.
        /// \return Filled container.
        template<template<class...> class ContainerT = std::set>
        ContainerT<KeyT> retrieve_all(const Transaction& txn) const {
            return retrieve_all<ContainerT>(txn.handle());
        }

        /// \brief Appends keys to the table.
        /// \tparam ContainerT Container type storing keys.
        /// \param container Source keys.
        /// \param txn Optional transaction handle.
        /// \note Duplicate source keys and keys already present in the table are
        ///       ignored by set semantics.
        template<template<class...> class ContainerT>
        void append(const ContainerT<KeyT>& container, MDBX_txn* txn = nullptr) {
            with_transaction([this, &container](MDBX_txn* t) {
                db_append(container, t);
            }, TransactionMode::WRITABLE, txn);
        }

        /// \brief Appends keys to the table.
        /// \tparam ContainerT Container type storing keys.
        /// \param container Source keys.
        /// \param txn Active transaction wrapper.
        template<template<class...> class ContainerT>
        void append(const ContainerT<KeyT>& container, const Transaction& txn) {
            append(container, txn.handle());
        }

        /// \brief Replaces table content with keys from a container.
        /// \tparam ContainerT Container type storing keys.
        /// \param container Source keys.
        /// \param txn Optional transaction handle.
        /// \note Clears existing table content before appending \p container.
        ///       This operation does not preserve existing records incrementally.
        template<template<class...> class ContainerT>
        void reconcile(const ContainerT<KeyT>& container, MDBX_txn* txn = nullptr) {
            with_transaction([this, &container](MDBX_txn* t) {
                db_clear(t);
                db_append(container, t);
            }, TransactionMode::WRITABLE, txn);
        }

        /// \brief Replaces table content with keys from a container.
        /// \tparam ContainerT Container type storing keys.
        /// \param container Source keys.
        /// \param txn Active transaction wrapper.
        template<template<class...> class ContainerT>
        void reconcile(const ContainerT<KeyT>& container, const Transaction& txn) {
            reconcile(container, txn.handle());
        }

        /// \brief Inserts a key if it is absent.
        /// \param key Key to insert.
        /// \param txn Optional transaction handle.
        /// \return \c true if inserted, \c false if the key already exists.
        /// \note Uses MDBX no-overwrite semantics; existing keys are never
        ///       replaced.
        bool insert(const KeyT& key, MDBX_txn* txn = nullptr) {
            bool res = false;
            with_transaction([this, &key, &res](MDBX_txn* t) {
                res = db_insert(key, t);
            }, TransactionMode::WRITABLE, txn);
            return res;
        }

        /// \brief Inserts a key if it is absent.
        /// \param key Key to insert.
        /// \param txn Active transaction wrapper.
        /// \return \c true if inserted, \c false if the key already exists.
        bool insert(const KeyT& key, const Transaction& txn) {
            return insert(key, txn.handle());
        }

        /// \brief Checks whether a key exists.
        /// \param key Key to look up.
        /// \param txn Optional transaction handle.
        /// \return \c true if the key exists.
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
        /// \return \c true if the key exists.
        bool contains(const KeyT& key, const Transaction& txn) const {
            return contains(key, txn.handle());
        }

        /// \brief Counts stored keys.
        /// \param txn Optional transaction handle.
        /// \return Number of keys in the table.
        std::size_t count(MDBX_txn* txn = nullptr) const {
            std::size_t res = 0;
            with_transaction([this, &res](MDBX_txn* t) {
                res = db_count(t);
            }, TransactionMode::READ_ONLY, txn);
            return res;
        }

        /// \brief Counts stored keys.
        /// \param txn Active transaction wrapper.
        /// \return Number of keys in the table.
        std::size_t count(const Transaction& txn) const {
            return count(txn.handle());
        }

        /// \brief Checks whether the table has no keys.
        /// \param txn Optional transaction handle.
        /// \return \c true if the table is empty.
        bool empty(MDBX_txn* txn = nullptr) const {
            return count(txn) == 0;
        }

        /// \brief Checks whether the table has no keys.
        /// \param txn Active transaction wrapper.
        /// \return \c true if the table is empty.
        bool empty(const Transaction& txn) const {
            return empty(txn.handle());
        }

        /// \brief Erases a key.
        /// \param key Key to remove.
        /// \param txn Optional transaction handle.
        /// \return \c true if the key was removed.
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
        /// \return \c true if the key was removed.
        bool erase(const KeyT& key, const Transaction& txn) {
            return erase(key, txn.handle());
        }

        /// \brief Removes all keys.
        /// \param txn Optional transaction handle.
        void clear(MDBX_txn* txn = nullptr) {
            with_transaction([this](MDBX_txn* t) {
                db_clear(t);
            }, TransactionMode::WRITABLE, txn);
        }

        /// \brief Removes all keys.
        /// \param txn Active transaction wrapper.
        void clear(const Transaction& txn) {
            clear(txn.handle());
        }

    private:
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

        static MDBX_val empty_value() {
            MDBX_val value;
            value.iov_base = nullptr;
            value.iov_len = 0;
            return value;
        }

        template<template<class...> class ContainerT>
        void db_load(ContainerT<KeyT>& container, MDBX_txn* txn) const {
            MDBX_cursor* cursor = nullptr;
            check_mdbx(mdbx_cursor_open(txn, m_dbi, &cursor), "Failed to open MDBX cursor");
            try {
                MDBX_val db_key, db_val;
                int rc = MDBX_SUCCESS;
                while ((rc = mdbx_cursor_get(cursor, &db_key, &db_val, MDBX_NEXT)) == MDBX_SUCCESS) {
                    container.insert(container.end(), deserialize_value<KeyT>(db_key));
                }
                if (rc != MDBX_NOTFOUND) {
                    check_mdbx(rc, "Failed to read key table");
                }
                mdbx_cursor_close(cursor);
            } catch (...) {
                mdbx_cursor_close(cursor);
                throw;
            }
        }

        template<template<class...> class ContainerT>
        void db_append(const ContainerT<KeyT>& container, MDBX_txn* txn) {
            for (typename ContainerT<KeyT>::const_iterator it = container.begin();
                 it != container.end(); ++it) {
                db_insert(*it, txn);
            }
        }

        bool db_insert(const KeyT& key, MDBX_txn* txn) {
            SerializeScratch sc_key;
            MDBX_val db_key = serialize_key<Options::safe_integer_key>(key, sc_key);
            MDBX_val db_val = empty_value();
            int rc = mdbx_put(txn, m_dbi, &db_key, &db_val, MDBX_NOOVERWRITE);
            if (rc == MDBX_SUCCESS) return true;
            if (rc == MDBX_KEYEXIST) return false;
            check_mdbx(rc, "Failed to insert key");
            return false;
        }

        bool db_contains(const KeyT& key, MDBX_txn* txn) const {
            SerializeScratch sc_key;
            MDBX_val db_key = serialize_key<Options::safe_integer_key>(key, sc_key);
            MDBX_val db_val;
            int rc = mdbx_get(txn, m_dbi, &db_key, &db_val);
            if (rc == MDBX_SUCCESS) return true;
            if (rc == MDBX_NOTFOUND) return false;
            check_mdbx(rc, "Failed to check key presence");
            return false;
        }

        std::size_t db_count(MDBX_txn* txn) const {
            MDBX_stat stat;
            check_mdbx(mdbx_dbi_stat(txn, m_dbi, &stat, sizeof(stat)), "Failed to query database statistics");
            return stat.ms_entries;
        }

        bool db_erase(const KeyT& key, MDBX_txn* txn) {
            SerializeScratch sc_key;
            MDBX_val db_key = serialize_key<Options::safe_integer_key>(key, sc_key);
            int rc = mdbx_del(txn, m_dbi, &db_key, nullptr);
            if (rc == MDBX_SUCCESS) return true;
            if (rc == MDBX_NOTFOUND) return false;
            check_mdbx(rc, "Failed to erase key");
            return false;
        }

        void db_clear(MDBX_txn* txn) {
            check_mdbx(mdbx_drop(txn, m_dbi, 0), "Failed to clear table");
        }
    };

} // namespace mdbxc

#endif // _MDBX_CONTAINERS_KEY_TABLE_HPP_INCLUDED
