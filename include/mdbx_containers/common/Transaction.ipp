#include <iostream>

namespace mdbxc {

    inline Transaction::Transaction(TransactionTracker* registry, MDBX_env* env, TransactionMode mode)
        : m_registry(registry), m_env(env), m_mode(mode) {
        begin();
    }

    inline Transaction::~Transaction() {
        std::cout << "u1" << std::endl;
	    m_registry->unbind_txn();
		std::cout << "u2" << std::endl;
        if (!m_txn) return;
		std::cout << "u3" << std::endl;
        mdbx_txn_abort(m_txn);
		std::cout << "u4" << std::endl;
        m_txn = nullptr;
    }

    inline void Transaction::begin() {
        if (m_started) return;
        if (m_txn && m_mode == TransactionMode::READ_ONLY) {
            check_mdbx(mdbx_txn_renew(m_txn), "Failed to renew transaction");
        } else {
            MDBX_txn_flags_t flags = (m_mode == TransactionMode::READ_ONLY) ? MDBX_TXN_RDONLY : MDBX_TXN_READWRITE;
            check_mdbx(mdbx_txn_begin(m_env, nullptr, flags, &m_txn), "Failed to begin transaction");
        }
        m_registry->bind_txn(m_txn);
        m_started = true;
    }

    inline void Transaction::commit() {
        if (!m_txn || !m_started) throw MdbxException("No active transaction to commit.");
        try {
            switch (m_mode) {
            case TransactionMode::READ_ONLY:
                check_mdbx(mdbx_txn_reset(m_txn), "Failed to reset read-only transaction");
                break;
            case TransactionMode::WRITABLE:
                check_mdbx(mdbx_txn_commit(m_txn), "Failed to commit writable transaction");
                m_txn = nullptr;
                m_registry->unbind_txn();
                break;
            };
            m_started = false;
        } catch (...) {
            if (m_txn) {
                mdbx_txn_abort(m_txn);
                m_txn = nullptr;
                m_registry->unbind_txn();
            }
            m_started = false;
            throw;
        }
    }

    inline void Transaction::rollback() {
        if (!m_txn || !m_started) throw MdbxException("No active transaction to rollback.");
        try {
            switch (m_mode) {
            case TransactionMode::READ_ONLY:
                check_mdbx(mdbx_txn_reset(m_txn), "Failed to reset read-only transaction");
                break;
            case TransactionMode::WRITABLE:
                check_mdbx(mdbx_txn_abort(m_txn), "Failed to abort writable transaction");
                m_txn = nullptr;
                m_registry->unbind_txn();
                break;
            };
            m_started = false;
        } catch (...) {
            if (m_txn) {
                mdbx_txn_abort(m_txn);
                m_txn = nullptr;
                m_registry->unbind_txn();
            }
            m_started = false;
            throw;
        }
    }

    inline MDBX_txn* Transaction::handle() const noexcept {
        return m_txn;
    }

} // namespace mdbxc