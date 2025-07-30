#pragma once
#ifndef _MDBX_CONTAINERS_EXCEPTION_HPP_INCLUDED
#define _MDBX_CONTAINERS_EXCEPTION_HPP_INCLUDED

/// \file MdbxException.hpp
/// \brief Defines a specific exception for MDBX-related errors.

namespace mdbxc {

    /// \class MdbxException
    /// \brief Represents a specific exception for MDBX-related errors.
    ///
    /// This exception is used to handle errors that occur during MDBX operations.
    /// It extends `std::runtime_error` and adds support for storing an MDBX-specific error code.
    class MdbxException : public std::runtime_error {
    public:
        /// \brief Constructs a new MdbxException with the given message and error code.
        /// \param message The error message describing the exception.
        /// \param error_code The MDBX error code associated with this exception (default: -1).
        explicit MdbxException(const std::string& message, int error_code = -1)
            : std::runtime_error("MDBXC error: " + message), m_error_code(error_code) {}

        /// \brief Returns the MDBX error code associated with this exception.
        /// \return The MDBX error code.
        int error_code() const noexcept {
            return m_error_code;
        }

    private:
        int m_error_code; ///< The MDBX error code associated with the exception.
    };

} // namespace mdbxc

#endif // _MDBX_CONTAINERS_EXCEPTION_HPP_INCLUDED