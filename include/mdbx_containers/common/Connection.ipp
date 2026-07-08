#include <string>
#include <memory>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <cassert>

#if __cplusplus >= 201703L
# include <filesystem>
#endif

#include <mdbx.h>

#if MDBXC_SYNC_ENABLED
#include "../sync/ISyncCaptureSink.hpp"
#endif

namespace mdbxc {

    inline Connection::Connection(const Config& config) {
        connect(config);
    }

    inline Connection::~Connection() {
        std::lock_guard<std::mutex> locker(m_mdbx_mutex);
        assert(!has_txn_handles() && "Destroying Connection with live transaction handles");
        cleanup(false);
    }

    inline std::shared_ptr<Connection> Connection::create(const Config& config) {
        auto conn = std::make_shared<Connection>();
        conn->connect(config);
        return conn;
    }

    inline void Connection::configure(const Config& config) {
        std::lock_guard<std::mutex> locker(m_mdbx_mutex);
#       if __cplusplus >= 201703L
        m_config = config;
#       else
        m_config.reset(new Config(config));
#       endif
    }

    inline void Connection::connect() {
        std::lock_guard<std::mutex> locker(m_mdbx_mutex);
        if (m_env) return;
        if (!m_config) throw std::logic_error("No configuration provided.");
        m_shutdown_requested = false;
        initialize();
    }

    inline void Connection::connect(const Config& config) {
        std::lock_guard<std::mutex> locker(m_mdbx_mutex);
        if (m_env) return;
#       if __cplusplus >= 201703L
        m_config = config;
#       else
        m_config.reset(new Config(config));
#       endif
        m_shutdown_requested = false;
        initialize();
    }

    inline void Connection::disconnect() {
        std::lock_guard<std::mutex> locker(m_mdbx_mutex);
        cleanup();
    }

    inline void Connection::shutdown() {
        if (!request_shutdown()) {
            return;
        }
        wait_for_no_txn_handles();
        disconnect();
    }

    inline bool Connection::is_connected() const {
        std::lock_guard<std::mutex> locker(m_mdbx_mutex);
        return m_env != nullptr;
    }

    inline bool Connection::is_read_only() const {
        std::lock_guard<std::mutex> locker(m_mdbx_mutex);
        return m_config ? m_config->read_only : false;
    }

    inline Transaction Connection::transaction(TransactionMode mode) {
        std::lock_guard<std::mutex> locker(m_mdbx_mutex);
        if (m_shutdown_requested) {
            throw std::logic_error("Connection shutdown is in progress.");
        }
        if (!m_env) {
            throw std::logic_error("Connection is not connected.");
        }
        return Transaction(static_cast<TransactionTracker*>(this), m_env, mode);
    }

    inline void Connection::begin(TransactionMode mode) {
        std::lock_guard<std::mutex> lock(m_mdbx_mutex);
        if (m_shutdown_requested) {
            throw std::logic_error("Connection shutdown is in progress.");
        }
        if (!m_env) {
            throw std::logic_error("Connection is not connected.");
        }
        auto tid = std::this_thread::get_id();
        auto it = m_transactions.find(tid);
        if (it != m_transactions.end()) {
            throw std::logic_error("Transaction already started for this thread.");
        }
        auto txn = std::make_shared<Transaction>(static_cast<TransactionTracker*>(this), m_env, mode);
        m_transactions[tid] = txn;
    }

    inline void Connection::commit() {
        std::lock_guard<std::mutex> lock(m_mdbx_mutex);
        auto tid = std::this_thread::get_id();
        auto it = m_transactions.find(tid);
        if (it == m_transactions.end()) {
            throw std::logic_error("No transaction for this thread.");
        }
        try {
            it->second->commit();
        } catch (...) {
            m_transactions.erase(it);
            throw;
        }
        m_transactions.erase(it);
    }

    inline void Connection::rollback() {
        std::lock_guard<std::mutex> lock(m_mdbx_mutex);
        auto tid = std::this_thread::get_id();
        auto it = m_transactions.find(tid);
        if (it == m_transactions.end()) {
            throw std::logic_error("No transaction for this thread.");
        }
        it->second->rollback();
        m_transactions.erase(it);
    }

    inline std::shared_ptr<Transaction> Connection::current_txn() const {
        std::lock_guard<std::mutex> lock(m_mdbx_mutex);
        auto it = m_transactions.find(std::this_thread::get_id());
        return (it != m_transactions.end()) ? it->second : nullptr;
    }

    inline MDBX_env* Connection::env_handle() noexcept {
        return m_env;
    }

    inline int64_t Connection::max_dupsort_value_size() const {
        std::lock_guard<std::mutex> locker(m_mdbx_mutex);
        return m_config ? m_config->max_dupsort_value_size : Config().max_dupsort_value_size;
    }

#if MDBXC_SYNC_ENABLED
    inline void Connection::attach_sync_capture(sync::ISyncCaptureSink* sink) {
        std::lock_guard<std::mutex> locker(m_mdbx_mutex);
        m_sync_capture = sink;
    }

    inline void Connection::detach_sync_capture() {
        std::lock_guard<std::mutex> locker(m_mdbx_mutex);
        m_sync_capture = nullptr;
    }

    inline sync::ISyncCaptureSink* Connection::sync_capture() const {
        return m_sync_capture;
    }

    inline void Connection::on_pre_commit(MDBX_txn* txn) {
        sync::ISyncCaptureSink* sink = sync_capture();
        if (sink != nullptr) {
            sink->flush_in_txn(txn);
        }
    }

    inline void Connection::on_discard(MDBX_txn* txn) noexcept {
        try {
            sync::ISyncCaptureSink* sink = sync_capture();
            if (sink != nullptr) {
                sink->discard_txn(txn);
            }
        } catch (...) {
            // discard must not throw; sink contract is noexcept.
        }
    }
#endif

    inline void Connection::backup_to(const std::string& path, const BackupOptions& options) {
        std::lock_guard<std::mutex> locker(m_mdbx_mutex);
        if (!m_env) {
            throw MdbxException("Connection is not connected.", MDBX_EINVAL);
        }
        if (m_shutdown_requested) {
            throw std::logic_error("Cannot backup during connection shutdown.");
        }

        MDBX_copy_flags_t flags = MDBX_CP_DEFAULTS;
        if (options.mode == BackupMode::Compact) {
            flags = static_cast<MDBX_copy_flags_t>(flags | MDBX_CP_COMPACT);
        }
        if (options.throttle_mvcc) {
            flags = static_cast<MDBX_copy_flags_t>(flags | MDBX_CP_THROTTLE_MVCC);
        }
        if (options.dont_flush) {
            flags = static_cast<MDBX_copy_flags_t>(flags | MDBX_CP_DONT_FLUSH);
        }
        if (options.force_dynamic_size) {
            flags = static_cast<MDBX_copy_flags_t>(flags | MDBX_CP_FORCE_DYNAMIC_SIZE);
        }

        const int rc = mdbx_env_copy(m_env, path.c_str(), flags);
        if (rc != MDBX_SUCCESS) {
            check_mdbx(rc, "mdbx_env_copy failed");
        }
    }

    inline void Connection::sync_to_disk(bool force, bool nonblock) {
        std::lock_guard<std::mutex> locker(m_mdbx_mutex);
        if (!m_env) {
            throw MdbxException("Connection is not connected.", MDBX_EINVAL);
        }
        const int rc = mdbx_env_sync_ex(m_env, force, nonblock);
        // mdbx_env_sync_ex returns MDBX_SUCCESS on flushed data and
        // MDBX_RESULT_TRUE when nothing was pending; both are success.
        if (rc != MDBX_SUCCESS && rc != MDBX_RESULT_TRUE) {
            check_mdbx(rc, "mdbx_env_sync_ex failed");
        }
    }

    inline void Connection::initialize() {
        try {
            db_init();
        } catch (...) {
            if (m_env && mdbx_env_close(m_env) == MDBX_SUCCESS) {
                m_env = nullptr;
            }
            throw;
        }
    }

    inline void Connection::cleanup(bool use_throw) {
        if (has_txn_handles()) {
            if (use_throw) {
                throw MdbxException("Cannot disconnect while transaction handles are open.", MDBX_BUSY);
            }
            return;
        }
        m_transactions.clear();
        if (m_env) {
            int rc = mdbx_env_close(m_env);
            if (rc != MDBX_SUCCESS && use_throw) {
                check_mdbx(rc, "Failed to close environment");
            }
            if (rc == MDBX_SUCCESS) {
                m_env = nullptr;
                m_shutdown_requested = false;
            }
        }
    }

    inline bool Connection::request_shutdown() {
        std::lock_guard<std::mutex> locker(m_mdbx_mutex);
        if (!m_env) {
            m_shutdown_requested = false;
            return false;
        }
        if (current_thread_has_txn_handle()) {
            throw std::logic_error("Cannot shutdown from a thread with an open transaction handle.");
        }
        m_shutdown_requested = true;
        return true;
    }
    
    inline void Connection::db_init() {
#if __cplusplus >= 201703L
        namespace fs = std::filesystem;
#endif

        int rc = 0;
        check_mdbx(
            mdbx_env_create(&m_env),
            "Failed to create environment"
        );

        check_mdbx(
            mdbx_env_set_geometry(
                m_env,
                m_config->size_lower,
                m_config->size_now,
                m_config->size_upper,
                m_config->growth_step,
                m_config->shrink_threshold,
                m_config->page_size
            ),
            "Failed to set environment geometry"
        );

        check_mdbx(
            mdbx_env_set_maxdbs(m_env, m_config->max_dbs),
            "Failed to set max databases"
        );

        int readers = m_config->max_readers > 0
            ? static_cast<int>(m_config->max_readers)
            : static_cast<int>(std::thread::hardware_concurrency()) * 2;
        check_mdbx(
            mdbx_env_set_maxreaders(m_env, readers),
            "Failed to set max readers"
        );

        MDBX_env_flags_t env_flags = MDBX_ACCEDE;
        if (m_config->no_subdir)     env_flags |= MDBX_NOSUBDIR;
        if (m_config->sync_durable)  env_flags |= MDBX_SYNC_DURABLE;
        if (m_config->read_only)     env_flags |= MDBX_RDONLY;
        if (!m_config->readahead)    env_flags |= MDBX_NORDAHEAD;
        if (m_config->writemap_mode) env_flags |= MDBX_WRITEMAP;

        std::string pathname = m_config->pathname;
        if (m_config->relative_to_exe && 
            !is_absolute_path(pathname) &&
            !is_explicitly_relative(pathname)) {
#       if __cplusplus >= 201703L
#           if __cplusplus >= 202002L
            pathname = u8string_to_string((fs::u8path(get_exec_dir()) / fs::u8path(pathname)).u8string());
#           else
            pathname = (fs::u8path(get_exec_dir()) / fs::u8path(pathname)).u8string();
#           endif
#       else
#           ifdef _WIN32
            pathname = get_exec_dir() + "\\" + pathname;
#           else
            pathname = get_exec_dir() + "/" + pathname;
#           endif
#       endif
        }
        if (!m_config->read_only) {
            create_directories(pathname);
        }

#ifdef _WIN32
#   if __cplusplus >= 201703L
#       if __cplusplus >= 202002L
        fs::path file_path = fs::u8path(pathname);
#       else
        std::wstring wide_path = utf8_to_wide(pathname);
        fs::path file_path = fs::path(wide_path);
#       endif
        file_path = file_path.lexically_normal();
        check_mdbx(
            mdbx_env_openW(m_env, file_path.c_str(), env_flags, 0664),
            "Failed to open environment"
        );
#   else
        pathname = lexically_normal_compat(pathname);
        std::wstring wide_path = utf8_to_wide(pathname);
        check_mdbx(
            mdbx_env_openW(m_env, wide_path.c_str(), env_flags, 0664),
            "Failed to open environment"
        );
#   endif

#else
        check_mdbx(
            mdbx_env_open(m_env, pathname.c_str(), env_flags, 0664),
            "Failed to open environment"
        );
#endif
    }

} // namespace mdbxc
