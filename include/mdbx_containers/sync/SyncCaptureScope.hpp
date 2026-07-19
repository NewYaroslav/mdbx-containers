#pragma once
#ifndef MDBX_CONTAINERS_HEADER_SYNC_SYNC_CAPTURE_SCOPE_HPP_INCLUDED
#define MDBX_CONTAINERS_HEADER_SYNC_SYNC_CAPTURE_SCOPE_HPP_INCLUDED

/// \file SyncCaptureScope.hpp
/// \brief RAII helper for temporarily attaching an \c ISyncCaptureSink.

#include "sync_module.hpp"

#if MDBXC_SYNC_ENABLED

#include "../common.hpp"
#include "ISyncCaptureSink.hpp"

#include <memory>
#include <stdexcept>

namespace mdbxc {
namespace sync {

    /// \brief Temporarily attaches a sync capture sink to a \c Connection.
    /// \details The scope stores the previously attached sink, attaches
    /// \p sink, and restores the previous sink on destruction or \c detach().
    /// The sink pointer is non-owning and must outlive the scope. This is a
    /// lifecycle helper; do not create or destroy it concurrently with table
    /// operations or active transactions on the same connection.
    class SyncCaptureScope {
    public:
        SyncCaptureScope(const std::shared_ptr<Connection>& connection,
                         ISyncCaptureSink& sink)
            : m_connection(connection),
              m_previous(nullptr),
              m_active(false) {
            attach(&sink);
        }

        SyncCaptureScope(const std::shared_ptr<Connection>& connection,
                         ISyncCaptureSink* sink)
            : m_connection(connection),
              m_previous(nullptr),
              m_active(false) {
            if (sink == nullptr) {
                throw std::invalid_argument("SyncCaptureScope sink is null");
            }
            attach(sink);
        }

        ~SyncCaptureScope() {
            detach();
        }

        SyncCaptureScope(const SyncCaptureScope&) = delete;
        SyncCaptureScope& operator=(const SyncCaptureScope&) = delete;

        /// \brief Restores the sink that was attached before this scope.
        /// \details Safe to call more than once.
        void detach() {
            if (!m_active) {
                return;
            }
            if (m_previous != nullptr) {
                m_connection->attach_sync_capture(m_previous);
            } else {
                m_connection->detach_sync_capture();
            }
            m_active = false;
        }

        /// \brief Returns whether this scope still owns the attachment.
        bool active() const {
            return m_active;
        }

    private:
        void attach(ISyncCaptureSink* sink) {
            if (!m_connection) {
                throw std::invalid_argument(
                    "SyncCaptureScope connection is null");
            }
            m_previous = m_connection->sync_capture();
            m_connection->attach_sync_capture(sink);
            m_active = true;
        }

        std::shared_ptr<Connection> m_connection;
        ISyncCaptureSink* m_previous;
        bool m_active;
    };

} // namespace sync
} // namespace mdbxc

#endif // MDBXC_SYNC_ENABLED

#endif // MDBX_CONTAINERS_HEADER_SYNC_SYNC_CAPTURE_SCOPE_HPP_INCLUDED
