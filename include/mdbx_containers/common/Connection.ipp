#include <string>
#include <memory>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <filesystem>

#if __cplusplus < 202002L
# include <codecvt>
# include <locale>
#endif

#include <mdbx.h>

namespace mdbxc {

    inline Connection::Connection(const Config& config) {
        connect(config);
    }

    inline Connection::~Connection() {
        std::lock_guard<std::mutex> locker(m_mdbx_mutex);
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
        initialize();
    }

    inline void Connection::disconnect() {
        std::lock_guard<std::mutex> locker(m_mdbx_mutex);
        cleanup();
    }

    inline bool Connection::is_connected() const {
        std::lock_guard<std::mutex> locker(m_mdbx_mutex);
        return m_env != nullptr;
    }

    inline Transaction Connection::transaction(TransactionMode mode) {
        return Transaction(static_cast<TransactionTracker*>(this), m_env, mode);
    }

    inline void Connection::begin(TransactionMode mode) {
        std::lock_guard<std::mutex> lock(m_mdbx_mutex);
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
        it->second->commit();
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

    inline void Connection::initialize() {
        try {
            create_directories(m_config->pathname);
            db_init();
        } catch (...) {
            if (m_env && mdbx_env_close(m_env) == MDBX_SUCCESS) {
                m_env = nullptr;
            }
            throw;
        }
    }

    inline void Connection::cleanup(bool use_throw) {
        if (m_env) {
            int rc = mdbx_env_close(m_env);
            if (rc != MDBX_SUCCESS && use_throw) {
                check_mdbx(rc, "Failed to close environment");
            }
            m_env = nullptr;
        }
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
        if (m_config->relative_to_exe && !is_absolute_path(pathname)) {
#if __cplusplus >= 201703L
#if __cplusplus >= 202002L
            pathname = u8string_to_string((fs::u8path(get_exec_dir()) / fs::u8path(pathname)).u8string());
#else
            pathname = (fs::u8path(get_exec_dir()) / fs::u8path(pathname)).u8string();
#endif
#else
#   ifdef _WIN32
            pathname = get_exec_dir() + "\\" + pathname;
#   else
            pathname = get_exec_dir() + "/" + pathname;
#   endif
#endif
        }

#ifdef _WIN32
#   if __cplusplus >= 201703L
#       if __cplusplus >= 202002L
        fs::path file_path = fs::u8path(pathname);
#       else
        std::wstring_convert<std::codecvt_utf8_utf16<wchar_t>> converter;
        std::wstring wide_path = converter.from_bytes(pathname);
        fs::path file_path = fs::path(wide_path);
#       endif
        check_mdbx(
            mdbx_env_openW(m_env, file_path.c_str(), env_flags, 0664),
            "Failed to open environment"
        );
#   else
        std::wstring_convert<std::codecvt_utf8_utf16<wchar_t>> converter;
        std::wstring wide_path = converter.from_bytes(pathname);
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
