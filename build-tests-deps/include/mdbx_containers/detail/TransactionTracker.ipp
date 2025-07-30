namespace mdbxc {

    inline void TransactionTracker::bind_txn(MDBX_txn* txn) {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_thread_txns[std::this_thread::get_id()] = txn;
    }

    inline void TransactionTracker::unbind_txn() {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_thread_txns.erase(std::this_thread::get_id());
    }

    inline MDBX_txn* TransactionTracker::thread_txn() const {
        std::lock_guard<std::mutex> lock(m_mutex);
        auto it = m_thread_txns.find(std::this_thread::get_id());
        return (it != m_thread_txns.end()) ? it->second : nullptr;
    }

} // namespace mdbxc