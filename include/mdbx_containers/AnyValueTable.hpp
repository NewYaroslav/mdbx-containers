#pragma once
#ifndef _MDBX_CONTAINERS_ANY_VALUE_TABLE_HPP_INCLUDED
#define _MDBX_CONTAINERS_ANY_VALUE_TABLE_HPP_INCLUDED

/// \file AnyValueTable.hpp
/// \brief Table storing values of arbitrary type indexed by key.

#include "common.hpp"

namespace mdbxc {

    /// \class AnyValueTable
    /// \ingroup mdbxc_tables
    /// \brief Table storing values of arbitrary type associated with a key.
    /// \tparam KeyT Type of the key used to access values.
    template <class KeyT>
    class AnyValueTable final : public BaseTable {
    public:
        /// \brief Constructs table using existing connection.
        /// \param conn Shared connection to the environment.
        /// \param name Name of the table.
        /// \param flags Additional MDBX flags.
        AnyValueTable(std::shared_ptr<Connection> conn,
                      std::string name = "any_store",
                      MDBX_db_flags_t flags = MDBX_DB_DEFAULTS | MDBX_CREATE)
            : BaseTable(std::move(conn), std::move(name), flags | get_mdbx_flags<KeyT>()) {}

        /// \brief Constructs table using configuration.
        /// \param cfg Configuration settings.
        /// \param name Name of the table.
        /// \param flags Additional MDBX flags.
        explicit AnyValueTable(const Config& cfg,
                               std::string name = "any_store",
                               MDBX_db_flags_t flags = MDBX_DB_DEFAULTS | MDBX_CREATE)
            : BaseTable(Connection::create(cfg), std::move(name), flags | get_mdbx_flags<KeyT>()) {}

        /// \brief Destructor.
        ~AnyValueTable() override = default;

        // --- Write ---

        /// \brief Set value for key, replacing existing value.
        /// \tparam T Type of value.
        /// \param key Key to update.
        /// \param value Value to store.
        /// \param txn Optional transaction handle.
        template <class T>
        void set(const KeyT& key, const T& value, MDBX_txn* txn = nullptr) {
            with_transaction([&](MDBX_txn* t){
                put_typed(key, value, true, t);
            }, TransactionMode::WRITABLE, txn);
        }

        /// \brief Set value using external transaction.
        template <class T>
        void set(const KeyT& key, const T& value, const Transaction& txn) {
            set(key, value, txn.handle());
        }

        /// \brief Insert value if key does not exist.
        /// \return true if inserted, false if key already exists.
        template <class T>
        bool insert(const KeyT& key, const T& value, MDBX_txn* txn = nullptr) {
            bool res = false;
            with_transaction([&](MDBX_txn* t){
                res = put_typed(key, value, false, t);
            }, TransactionMode::WRITABLE, txn);
            return res;
        }

        /// \brief Insert value using external transaction.
        template <class T>
        bool insert(const KeyT& key, const T& value, const Transaction& txn) {
            return insert<T>(key, value, txn.handle());
        }

        /// \brief Update value using functor.
        /// \tparam T Expected type of stored value.
        /// \tparam Fn Functor accepting reference to value.
        /// \param key Key to modify.
        /// \param fn Function applied to the value.
        /// \param create_if_missing Create default value if key is absent.
        /// \param txn Optional transaction handle.
        template <class T, class Fn>
        void update(const KeyT& key, Fn&& fn, bool create_if_missing = false, MDBX_txn* txn = nullptr) {
            with_transaction([&](MDBX_txn* t){
                T tmp{};
                bool exists = get_typed(key, tmp, t);
                if (!exists) {
                    if (!create_if_missing) {
                        throw std::out_of_range("Key not found");
                    }
                }
                fn(tmp);
                put_typed(key, tmp, true, t);
            }, TransactionMode::WRITABLE, txn);
        }

        /// \brief Update using external transaction.
        template <class T, class Fn>
        void update(const KeyT& key, Fn&& fn, bool create_if_missing, const Transaction& txn) {
            update<T, Fn>(key, std::forward<Fn>(fn), create_if_missing, txn.handle());
        }

        // --- Read ---

        /// \brief Retrieve stored value or throw if missing.
        /// \param key Key to look up.
        /// \param txn Optional transaction handle.
        /// \throws std::out_of_range if key not found.
        /// \throws std::bad_cast if type tag check fails.
        template <class T>
        T get(const KeyT& key, MDBX_txn* txn = nullptr) const {
            T out{};
            bool found = false;
            with_transaction([&](MDBX_txn* t){
                found = get_typed(key, out, t);
            }, TransactionMode::READ_ONLY, txn);
            if (!found) {
                throw std::out_of_range("Key not found");
            }
            return out;
        }

        /// \brief Retrieve using external transaction.
        template <class T>
        T get(const KeyT& key, const Transaction& txn) const {
            return get<T>(key, txn.handle());
        }

        /// \brief Find value by key.
        /// \return Optional with value or std::nullopt.
#if __cplusplus >= 201703L
        template <class T>
        std::optional<T> find(const KeyT& key, MDBX_txn* txn = nullptr) const {
            std::optional<T> result;
            with_transaction([&](MDBX_txn* t){
                T tmp{};
                try {
                    if (get_typed(key, tmp, t)) {
                        result = std::move(tmp);
                    }
                } catch (const std::bad_cast&) {
                    // type mismatch -> treat as not found
                }
            }, TransactionMode::READ_ONLY, txn);
            return result;
        }

        /// \brief Find using external transaction.
        template <class T>
        std::optional<T> find(const KeyT& key, const Transaction& txn) const {
            return find<T>(key, txn.handle());
        }

        /// \brief Get value or default if missing.
        /// \param key Key to look up.
        /// \param default_value Value returned when key not found.
        template <class T>
        T get_or(const KeyT& key, T default_value, MDBX_txn* txn = nullptr) const {
            if (auto val = find<T>(key, txn)) {
                return *std::move(val);
            }
            return default_value;
        }

        /// \brief Get value or default using external transaction.
        template <class T>
        T get_or(const KeyT& key, T default_value, const Transaction& txn) const {
            return get_or<T>(key, std::move(default_value), txn.handle());
        }
#else
        template <class T>
        std::pair<bool, T> find_compat(const KeyT& key, MDBX_txn* txn = nullptr) const {
            std::pair<bool, T> result{false, T{}};
            with_transaction([&](MDBX_txn* t){
                try {
                    if (get_typed(key, result.second, t)) {
                        result.first = true;
                    }
                } catch (const std::bad_cast&) {
                    // type mismatch -> treat as not found
                }
            }, TransactionMode::READ_ONLY, txn);
            return result;
        }

        template <class T>
        std::pair<bool, T> find_compat(const KeyT& key, const Transaction& txn) const {
            return find_compat<T>(key, txn.handle());
        }

        template <class T>
        T get_or(const KeyT& key, T default_value, MDBX_txn* txn = nullptr) const {
            auto res = find_compat<T>(key, txn);
            if (res.first) {
                return std::move(res.second);
            }
            return default_value;
        }

        template <class T>
        T get_or(const KeyT& key, T default_value, const Transaction& txn) const {
            return get_or<T>(key, std::move(default_value), txn.handle());
        }
#endif

        // --- Meta ---

        /// \brief Check if key exists.
        template <class KT = KeyT>
        bool contains(const KT& key, MDBX_txn* txn = nullptr) const {
            bool res = false;
            with_transaction([&](MDBX_txn* t){ res = db_contains(key, t); }, TransactionMode::READ_ONLY, txn);
            return res;
        }

        /// \brief Check key existence using external transaction.
        bool contains(const KeyT& key, const Transaction& txn) const {
            return contains(key, txn.handle());
        }

        /// \brief Erase key from table.
        bool erase(const KeyT& key, MDBX_txn* txn = nullptr) {
            bool res = false;
            with_transaction([&](MDBX_txn* t){ res = db_erase(key, t); }, TransactionMode::WRITABLE, txn);
            return res;
        }

        /// \brief Erase using external transaction.
        bool erase(const KeyT& key, const Transaction& txn) {
            return erase(key, txn.handle());
        }

        /// \brief List all keys stored in table.
        std::vector<KeyT> keys(MDBX_txn* txn = nullptr) const {
            std::vector<KeyT> out;
            with_transaction([&](MDBX_txn* t){ db_list_keys(out, t); }, TransactionMode::READ_ONLY, txn);
            return out;
        }

        /// \brief List keys using external transaction.
        std::vector<KeyT> keys(const Transaction& txn) const {
            return keys(txn.handle());
        }

        /// \brief Enable or disable type-tag checking.
        void set_type_tag_check(bool enabled) noexcept { m_check_type_tag = enabled; }

    private:
        bool m_check_type_tag = false; ///< Flag enabling type-tag verification.

        template<typename F>
        void with_transaction(F&& action, TransactionMode mode, MDBX_txn* txn) const {
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

        template <class T>
        bool put_typed(const KeyT& key, const T& value, bool upsert, MDBX_txn* txn) {
            SerializeScratch sc_key;
            SerializeScratch sc_value;
            MDBX_val db_key = serialize_key(key, sc_key);
            MDBX_val raw_val = serialize_value(value, sc_value);
            MDBX_val db_val = wrap_with_type_tag<T>(raw_val);
            MDBX_put_flags_t flags = upsert ? MDBX_UPSERT : MDBX_NOOVERWRITE;
            int rc = mdbx_put(txn, m_dbi, &db_key, &db_val, flags);
            if (!upsert && rc == MDBX_KEYEXIST) {
                return false;
            }
            check_mdbx(rc, upsert ? "Failed to set value" : "Failed to insert value");
            return true;
        }

        template <class T>
        bool get_typed(const KeyT& key, T& out, MDBX_txn* txn) const {
            SerializeScratch sc_key;
            MDBX_val db_key = serialize_key(key, sc_key);
            MDBX_val db_val{};
            int rc = mdbx_get(txn, m_dbi, &db_key, &db_val);
            if (rc == MDBX_NOTFOUND) return false;
            check_mdbx(rc, "Failed to retrieve value");
            MDBX_val checked = unwrap_and_check_type_tag<T>(db_val);
            out = deserialize_value<T>(checked);
            return true;
        }

        bool db_contains(const KeyT& key, MDBX_txn* txn) const {
            SerializeScratch sc_key;
            MDBX_val db_key = serialize_key(key, sc_key);
            int rc = mdbx_get(txn, m_dbi, &db_key, nullptr);
            if (rc == MDBX_SUCCESS) return true;
            if (rc == MDBX_NOTFOUND) return false;
            check_mdbx(rc, "Failed to check key presence");
            return false;
        }

        bool db_erase(const KeyT& key, MDBX_txn* txn) {
            SerializeScratch sc_key;
            MDBX_val db_key = serialize_key(key, sc_key);
            int rc = mdbx_del(txn, m_dbi, &db_key, nullptr);
            if (rc == MDBX_SUCCESS) return true;
            if (rc == MDBX_NOTFOUND) return false;
            check_mdbx(rc, "Failed to erase key");
            return false;
        }

        void db_list_keys(std::vector<KeyT>& out, MDBX_txn* txn) const {
            MDBX_cursor* cursor = nullptr;
            check_mdbx(mdbx_cursor_open(txn, m_dbi, &cursor), "Failed to open MDBX cursor");
            MDBX_val db_key, db_val;
            while (mdbx_cursor_get(cursor, &db_key, &db_val, MDBX_NEXT) == MDBX_SUCCESS) {
                out.emplace_back(deserialize_value<KeyT>(db_key));
            }
            mdbx_cursor_close(cursor);
        }

        template <class T>
        MDBX_val wrap_with_type_tag(const MDBX_val& raw) const {
            if (!m_check_type_tag) return raw;
            return raw; // TODO: implement type-tag prefix
        }

        template <class T>
        MDBX_val unwrap_and_check_type_tag(const MDBX_val& raw) const {
            if (!m_check_type_tag) return raw;
            return raw; // TODO: verify type-tag
        }
    };

} // namespace mdbxc

#endif // _MDBX_CONTAINERS_ANY_VALUE_TABLE_HPP_INCLUDED
