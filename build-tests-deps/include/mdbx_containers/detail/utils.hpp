#pragma once
#ifndef _MDBX_CONTAINERS_UTILS_HPP_INCLUDED
#define _MDBX_CONTAINERS_UTILS_HPP_INCLUDED

/// \file utils.hpp
/// \brief Utility helper functions for serializing values to and from MDBX.
///        See: https://libmdbx.dqdkfa.ru/

/// \defgroup mdbxc_utils Utility functions
/// \brief Helper traits and serialization routines used by the library.
/// @{

namespace mdbxc {
    
    /// \brief Throws an MdbxException if MDBX return code indicates an error.
    /// \param rc      Return code from an MDBX function.
    /// \param context Description of the calling context.
    void check_mdbx(int rc, const std::string& context) {
        if (rc != MDBX_SUCCESS) {
            throw MdbxException(context + ": (" + std::to_string(rc) + ") " + std::string(mdbx_strerror(rc)), rc);
        }
    }
    
    // --- Traits --- 

    /// \brief Trait to check if a type provides a `to_bytes()` member.
    /// \tparam T Type under inspection.
    template <typename T>
    struct has_to_bytes {
    private:
        template <typename U>
        static auto check(U*) -> decltype(std::declval<const U>().to_bytes(), std::true_type());
        template <typename>
        static std::false_type check(...);
    public:
        static const bool value = decltype(check<T>(0))::value;
    };

    /// \brief Trait to check if a type provides a static `from_bytes()` method.
    /// \tparam T Type under inspection.
    template <typename T>
    struct has_from_bytes {
    private:
        template <typename U>
        static auto check(U*) -> decltype(U::from_bytes((const void*)0, size_t(0)), std::true_type());
        template <typename>
        static std::false_type check(...);
    public:
        static const bool value = decltype(check<T>(0))::value;
    };

    /// \brief Trait indicating that a container defines `value_type`.
    /// \tparam T Container type.
    template <typename T>
    struct has_value_type {
    private:
        template <typename U>
        static auto check(U*) -> decltype(typename U::value_type(), std::true_type());
        template <typename>
        static std::false_type check(...);
    public:
        static const bool value = decltype(check<T>(0))::value;
    };

//-----------------------------------------------------------------------------
    
    /// \brief Returns MDBX flags for a given key type.
    /// \tparam T Key type.
    /// \return MDBX_INTEGERKEY if \c T is an integer-like type; 0 otherwise.
    template<typename T>
    inline MDBX_db_flags_t get_mdbx_flags() {
        return
            std::is_same<T, int>::value || std::is_same<T, int32_t>::value ||
            std::is_same<T, uint32_t>::value || std::is_same<T, int64_t>::value ||
            std::is_same<T, uint64_t>::value || std::is_same<T, float>::value ||
            std::is_same<T, double>::value || std::is_same<T, char>::value ||
            std::is_same<T, unsigned char>::value
            ? MDBX_INTEGERKEY : static_cast<MDBX_db_flags_t>(0);
    }
    
//-----------------------------------------------------------------------------
    
    /// \brief Returns the size in bytes of a given key type.
    /// \tparam T Key type.
    /// \param key The key value.
    /// \return Size in bytes suitable for filling MDBX_val.
    template<typename T>
    size_t get_key_size(const T& key) {
        // std::string
        if (std::is_same<T, std::string>::value) {
            return key.size();
        }

        // 32-bit types
        if (std::is_same<T, int>::value ||
            std::is_same<T, int32_t>::value ||
            std::is_same<T, uint32_t>::value ||
            std::is_same<T, float>::value) {
            return sizeof(uint32_t);
        }

        // 64-bit types
        if (std::is_same<T, int64_t>::value ||
            std::is_same<T, uint64_t>::value ||
            std::is_same<T, double>::value) {
            return sizeof(uint64_t);
        }

        // byte vectors
        if (std::is_same<T, std::vector<std::byte> >::value ||
            std::is_same<T, std::vector<uint8_t> >::value ||
            std::is_same<T, std::vector<char> >::value ||
            std::is_same<T, std::vector<unsigned char> >::value) {
            return key.size();
        }

        // fallback
        return sizeof(T);
    }

    // --- serialize_key overloads ---
    
    /// \brief Serializes a key into MDBX_val for database operations.
    /// \tparam T Key type.
    /// \param key The key to convert.
    /// \return MDBX_val representing the key.
    template <typename T>
    typename std::enable_if<!has_to_bytes<T>::value && !std::is_same<T, std::string>::value && !std::is_trivially_copyable<T>::value, MDBX_val>::type
    serialize_key(const T& key) {
        static_assert(sizeof(T) == 0, "Unsupported type for serialize_key");
        MDBX_val val;
        val.iov_base = nullptr;
        val.iov_len  = 0;
        return val;
    }

    /// \brief Serializes a key of type std::string.
    /// \tparam T Must be std::string.
    template<typename T>
    typename std::enable_if<std::is_same<T, std::string>::value, MDBX_val>::type
    serialize_key(const T& key) {
        MDBX_val val;
        val.iov_base = const_cast<char*>(key.data());
        val.iov_len  = key.size();
        return val;
    }

    /// \brief Serializes a key stored in a byte vector.
    /// \tparam T Vector type containing bytes.
    template<typename T>
    typename std::enable_if<
        std::is_same<T, std::vector<std::byte>>::value ||
        std::is_same<T, std::vector<uint8_t>>::value ||
        std::is_same<T, std::vector<char>>::value ||
        std::is_same<T, std::vector<unsigned char>>::value, MDBX_val>::type
    serialize_key(const T& key) {
        MDBX_val val;
        val.iov_base = const_cast<void*>(static_cast<const void*>(key.data()));
        val.iov_len  = key.size();
        return val;
    }
    
    /// \brief Serializes a small integral key (<=16 bits).
    /// \tparam T Integral type.
    template<typename T>
    typename std::enable_if<
        std::is_integral<T>::value &&
        (sizeof(T) <= 2), MDBX_val>::type
    serialize_key(const T& key) {
        static_assert(sizeof(uint32_t) == 4, "Expected 4-byte wrapper");
        using storage_t = typename std::aligned_storage<sizeof(uint32_t), alignof(uint32_t)>::type;
        thread_local storage_t storage;
        *reinterpret_cast<uint32_t*>(&storage) = static_cast<uint32_t>(key);
        MDBX_val val;
        val.iov_len = static_cast<size_t>(sizeof(uint32_t));
        val.iov_base = static_cast<void*>(&storage);
        return val;
    }

    /// \brief Serializes a 32-bit integral or float key.
    /// \tparam T Supported 32-bit type.
    template<typename T>
    typename std::enable_if<
        std::is_same<T, int32_t>::value ||
        std::is_same<T, uint32_t>::value ||
        std::is_same<T, float>::value, MDBX_val>::type
    serialize_key(const T& key) {
        static_assert(sizeof(uint32_t) == 4, "Expected 4-byte integer");
        MDBX_val val;
#       if MDBXC_SAFE_INTEGERKEY
        using storage_t = typename std::aligned_storage<sizeof(uint32_t), alignof(uint32_t)>::type;
        static thread_local storage_t buffer;
        *reinterpret_cast<uint32_t*>(&buffer) = *reinterpret_cast<const uint32_t*>(&key);
        val.iov_base = static_cast<void*>(&buffer);
#       else
        val.iov_base = const_cast<void*>(static_cast<const void*>(&key));
#       endif
        val.iov_len = static_cast<size_t>(sizeof(uint32_t));
        return val;
    }

    /// \brief Serializes a 64-bit integral or double key.
    /// \tparam T Supported 64-bit type.
    template<typename T>
    typename std::enable_if<
        std::is_same<T, int64_t>::value ||
        std::is_same<T, uint64_t>::value ||
        std::is_same<T, double>::value, MDBX_val>::type
    serialize_key(const T& key) {
        static_assert(sizeof(uint64_t) == 8, "Expected 8-byte integer");
        MDBX_val val;
#       if MDBXC_SAFE_INTEGERKEY
        using storage_t = typename std::aligned_storage<sizeof(uint64_t), alignof(uint64_t)>::type;
        static thread_local storage_t buffer;
        *reinterpret_cast<uint64_t*>(&buffer) = *reinterpret_cast<const uint64_t*>(&key);
        val.iov_base = static_cast<void*>(&buffer);
#       else
        val.iov_base = const_cast<void*>(static_cast<const void*>(&key));
#       endif
        val.iov_len  = sizeof(uint64_t);
        return val;
    }

    /// \brief Serializes any other trivially copyable key type.
    /// \tparam T Trivially copyable type.
    template<typename T>
    typename std::enable_if<
        std::is_trivially_copyable<T>::value &&
        !std::is_same<T, std::string>::value &&
        !(std::is_integral<T>::value && sizeof(T) <= 2) &&
        !std::is_same<T, int32_t>::value &&
        !std::is_same<T, uint32_t>::value &&
        !std::is_same<T, float>::value &&
        !std::is_same<T, int64_t>::value &&
        !std::is_same<T, uint64_t>::value &&
        !std::is_same<T, double>::value, MDBX_val>::type
    serialize_key(const T& key) {
        MDBX_val val;
        val.iov_base = const_cast<void*>(static_cast<const void*>(&key));
        val.iov_len  = sizeof(T);
        return val;
    }

    /// \brief Serializes a std::bitset as a key.
    /// \tparam N Number of bits in the bitset.
    /// \param data Bitset value to serialize.
    template <size_t N>
    inline MDBX_val serialize_key(const std::bitset<N>& data) {
        const size_t num_bytes = (N + 7) / 8;
        static thread_local std::array<uint8_t, (N + 7) / 8> buffer;
        buffer.fill(0);
        for (size_t i = 0; i < N; ++i) {
            if (data[i]) buffer[i / 8] |= (1 << (i % 8));
        }
        MDBX_val val;
        val.iov_base = static_cast<void*>(buffer.data());
        val.iov_len  = num_bytes;
        return val;
    }

    // --- serialize_value overloads ---

    /// \brief Serializes a general value into MDBX_val.
    /// \tparam T Type of the value.
    /// \param value The value to serialize.
    /// \return MDBX_val structure with binary representation of the value.
    template <typename T>
    typename std::enable_if<
        !has_value_type<T>::value &&
        !std::is_same<T, std::vector<typename T::value_type>>::value &&
        !std::is_trivially_copyable<typename T::value_type>::value &&
        !has_to_bytes<T>::value && 
        !std::is_same<T, std::string>::value &&
        !std::is_trivially_copyable<T>::value, MDBX_val>::type
    serialize_value(const T& value) {
        static_assert(sizeof(T) == 0, "Unsupported type for serialize_value");
        MDBX_val val;
        val.iov_base = nullptr;
        val.iov_len  = 0;
        return val;
    }

    /// \brief Serializes a std::string value.
    /// \tparam T Must be std::string.
    template<typename T>
    typename std::enable_if<std::is_same<T, std::string>::value, MDBX_val>::type
    serialize_value(const T& value) {
        MDBX_val val;
        val.iov_base = const_cast<char*>(value.data());
        val.iov_len  = value.size();
        return val;
    }
    
    /// \brief Serializes containers (vector, deque, list, set) of trivially copyable elements.
    /// \tparam T Container type with value_type.
    /// \param container The container to serialize.
    template <typename T>
    typename std::enable_if<
        has_value_type<T>::value &&
        std::is_trivially_copyable<typename T::value_type>::value &&
        (
            //std::is_same<T, std::vector<typename T::value_type>>::value ||
            std::is_same<T, std::deque<typename T::value_type>>::value ||
            std::is_same<T, std::list<typename T::value_type>>::value ||
            std::is_same<T, std::set<typename T::value_type>>::value
        ),
        MDBX_val>::type
    serialize_value(const T& container) {
        using Elem = typename T::value_type;
        static thread_local std::vector<Elem> buffer;
        buffer.assign(container.begin(), container.end());

        MDBX_val val;
        val.iov_base = static_cast<void*>(buffer.data());
        val.iov_len  = buffer.size() * sizeof(Elem);
        return val;
    }

    /// \brief Serializes a vector of trivially copyable elements.
    /// \tparam T Vector type.
    template<typename T>
    typename std::enable_if<
        has_value_type<T>::value &&
        std::is_same<T, std::vector<typename T::value_type>>::value &&
        std::is_trivially_copyable<typename T::value_type>::value,
        MDBX_val>::type
    serialize_value(const T& value) {
        typedef typename T::value_type Elem;
        MDBX_val val;
        val.iov_base = const_cast<void*>(static_cast<const void*>(value.data()));
        val.iov_len  = value.size() * sizeof(Elem);
        return val;
    }

    /// \brief Serializes a value using its `to_bytes()` method.
    /// \tparam T Type providing `to_bytes`.
    template<typename T>
    typename std::enable_if<has_to_bytes<T>::value, MDBX_val>::type
    serialize_value(const T& value) {
        static thread_local std::vector<uint8_t> buffer;
        buffer = value.to_bytes();
        MDBX_val val;
        val.iov_base = static_cast<void*>(buffer.data());
        val.iov_len  = buffer.size();
        return val;
    }

    /// \brief Serializes any trivially copyable value.
    /// \tparam T Trivially copyable type.
    template<typename T>
    typename std::enable_if<
        !has_to_bytes<T>::value &&
        std::is_trivially_copyable<T>::value,
        MDBX_val>::type
    serialize_value(const T& value) {
        MDBX_val val;
        val.iov_base = const_cast<void*>(static_cast<const void*>(&value));
        val.iov_len  = sizeof(T);
        return val;
    }

    /// \brief Serializes a container of strings.
    /// \tparam T Container type with `std::string` elements.
    template<typename T>
    typename std::enable_if<
            has_value_type<T>::value &&
            std::is_same<typename T::value_type, std::string>::value,
            MDBX_val>::type
    serialize_value(const T& container) {
        static thread_local std::vector<uint8_t> buffer;
        buffer.clear();

        for (const auto& str : container) {
            uint32_t len = static_cast<uint32_t>(str.size());
            buffer.insert(buffer.end(),
                          reinterpret_cast<const uint8_t*>(&len),
                          reinterpret_cast<const uint8_t*>(&len) + sizeof(uint32_t));
            buffer.insert(buffer.end(), str.begin(), str.end());
        }

        MDBX_val val;
        val.iov_base = buffer.data();
        val.iov_len = buffer.size();
        return val;
    }

    // --- deserialize_value overloads ---
    
    /// \brief Deserializes a value from MDBX_val into type \c T.
    /// \tparam T Desired type.
    /// \param val MDBX_val containing raw data.
    /// \return Deserialized T.
    template<typename T>
    typename std::enable_if<
        !has_from_bytes<T>::value && 
        !std::is_same<T, std::string>::value && 
        !std::is_trivially_copyable<T>::value, T>::type
    deserialize_value(const MDBX_val& val) {
        static_assert(sizeof(T) == 0, "Unsupported type for deserialize_value");
        T out;
        return out;
    }

    /// \brief Deserializes a std::string value.
    /// \tparam T Must be std::string.
    template<typename T>
    typename std::enable_if<std::is_same<T, std::string>::value, T>::type
    deserialize_value(const MDBX_val& val) {
        return std::string(static_cast<const char*>(val.iov_base), val.iov_len);
    }

    /// \brief Deserializes a vector of bytes.
    /// \tparam T Vector type containing bytes.
    template<typename T>
    typename std::enable_if<
        std::is_same<T, std::vector<std::byte>>::value ||
        std::is_same<T, std::vector<uint8_t>>::value ||
        std::is_same<T, std::vector<char>>::value ||
        std::is_same<T, std::vector<unsigned char>>::value, T>::type
    deserialize_value(const MDBX_val& val) {
        const uint8_t* ptr = static_cast<const uint8_t*>(val.iov_base);
        return T(ptr, ptr + val.iov_len);
    }

    /// \brief Deserializes a deque of bytes.
    /// \tparam T Byte deque type.
    template<typename T>
    typename std::enable_if<
        std::is_same<T, std::deque<std::byte>>::value ||
        std::is_same<T, std::deque<uint8_t>>::value ||
        std::is_same<T, std::deque<char>>::value ||
        std::is_same<T, std::deque<unsigned char>>::value, T>::type
    deserialize_value(const MDBX_val& val) {
        const uint8_t* ptr = static_cast<const uint8_t*>(val.iov_base);
        return T(ptr, ptr + val.iov_len);
    }

    /// \brief Deserializes a list of bytes.
    /// \tparam T Byte list type.
    template<typename T>
    typename std::enable_if<
        std::is_same<T, std::list<std::byte>>::value ||
        std::is_same<T, std::list<uint8_t>>::value ||
        std::is_same<T, std::list<char>>::value ||
        std::is_same<T, std::list<unsigned char>>::value, T>::type
    deserialize_value(const MDBX_val& val) {
        const uint8_t* ptr = static_cast<const uint8_t*>(val.iov_base);
        return T(ptr, ptr + val.iov_len);
    }

    /// \brief Deserializes a vector of trivially copyable elements.
    /// \tparam T Vector type.
    template<typename T>
    typename std::enable_if<
        has_value_type<T>::value &&
        std::is_same<T, std::vector<typename T::value_type>>::value &&
        std::is_trivially_copyable<typename T::value_type>::value, T>::type
    deserialize_value(const MDBX_val& val) {
        typedef typename T::value_type Elem;
        if (val.iov_len % sizeof(Elem) != 0)
            throw std::runtime_error("deserialize_value: size not aligned");
        const size_t count = val.iov_len / sizeof(Elem);
        const Elem* data = static_cast<const Elem*>(val.iov_base);
        return T(data, data + count);
    }

    /// \brief Deserializes a deque or list of trivially copyable elements.
    /// \tparam T Container type.
    template<typename T>
    typename std::enable_if<
        (std::is_same<T, std::deque<typename T::value_type>>::value ||
         std::is_same<T, std::list<typename T::value_type>>::value) &&
        std::is_trivially_copyable<typename T::value_type>::value, T>::type
    deserialize_value(const MDBX_val& val) {
        typedef typename T::value_type Elem;
        if (val.iov_len % sizeof(Elem) != 0)
            throw std::runtime_error("deserialize_value: size not aligned");
        const size_t count = val.iov_len / sizeof(Elem);
        const Elem* data = static_cast<const Elem*>(val.iov_base);
        return T(data, data + count);
    }

    /// \brief Deserializes a trivially copyable value.
    /// \tparam T Trivially copyable type.
    template<typename T>
    typename std::enable_if<
        !has_from_bytes<T>::value && std::is_trivially_copyable<T>::value, T>::type
    deserialize_value(const MDBX_val& val) {
        if (val.iov_len != sizeof(T))
            throw std::runtime_error("deserialize_value: size mismatch");
        T out;
        std::memcpy(&out, val.iov_base, sizeof(T));
        return out;
    }

    /// \brief Deserializes a value using its `from_bytes()` method.
    /// \tparam T Type providing `from_bytes`.
    template<typename T>
    typename std::enable_if<has_from_bytes<T>::value, T>::type
    deserialize_value(const MDBX_val& val) {
        return T::from_bytes(val.iov_base, val.iov_len);
    }

    /// \brief Deserializes a container of strings.
    /// \tparam T Container type with `std::string` elements.
    template<typename T>
    typename std::enable_if<
            has_value_type<T>::value &&
            std::is_same<typename T::value_type, std::string>::value,
            T>::type
    deserialize_value(const MDBX_val& val) {
        const uint8_t* ptr = static_cast<const uint8_t*>(val.iov_base);
        const uint8_t* end = ptr + val.iov_len;

        T result;
        while (ptr + sizeof(uint32_t) <= end) {
            uint32_t len;
            std::memcpy(&len, ptr, sizeof(uint32_t));
            ptr += sizeof(uint32_t);

            if (ptr + len > end)
                throw std::runtime_error("deserialize_value: corrupted data (length overflow)");

            result.emplace_back(reinterpret_cast<const char*>(ptr), len);
            ptr += len;
        }

        if (ptr != end)
            throw std::runtime_error("deserialize_value: trailing data after deserialization");

        return result;
    }
    
    /// \brief Deserializes a set of strings.
    /// \tparam T Either `std::set<std::string>` or `std::unordered_set<std::string>`.
    template<typename T>
    typename std::enable_if<
            std::is_same<T, std::set<std::string>>::value ||
            std::is_same<T, std::unordered_set<std::string>>::value,
            T>::type
    deserialize_value(const MDBX_val& val) {
        const uint8_t* ptr = static_cast<const uint8_t*>(val.iov_base);
        const uint8_t* end = ptr + val.iov_len;
        T result;

        while (ptr + sizeof(uint32_t) <= end) {
            uint32_t len;
            std::memcpy(&len, ptr, sizeof(uint32_t));
            ptr += sizeof(uint32_t);

            if (ptr + len > end)
                throw std::runtime_error("deserialize_value: corrupted data (length overflow)");

            result.insert(std::string(reinterpret_cast<const char*>(ptr), len));
            ptr += len;
        }

        if (ptr != end)
            throw std::runtime_error("deserialize_value: trailing data after deserialization");

        return result;
    }

}; // namespace mdbxc

/// @}

#endif // _MDBX_CONTAINERS_UTILS_HPP_INCLUDED
