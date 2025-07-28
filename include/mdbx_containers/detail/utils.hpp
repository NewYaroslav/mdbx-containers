#pragma once
#ifndef _MDBX_CONTAINERS_UTILS_HPP_INCLUDED
#define _MDBX_CONTAINERS_UTILS_HPP_INCLUDED

/// \file utils.hpp
/// \brief Utility functions for working with MDBX using 32/64-bit keys.
/// See: https://libmdbx.dqdkfa.ru/

namespace mdbxc {
    
    /// \brief Throws an MdbxException if MDBX return code is not success.
    void check_mdbx(int rc, const std::string& context) {
        if (rc != MDBX_SUCCESS) {
            throw MdbxException(context + ": (" + std::to_string(rc) + ") " + std::string(mdbx_strerror(rc)), rc);
        }
    }
    
    // --- Traits --- 

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
    /// \return MDBX_INTEGERKEY if T is an integer-like type; 0 otherwise.
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
    /// \param key The key to convert.
    /// \return MDBX_val representing the key.
    template <typename T>
    typename std::enable_if<!has_to_bytes<T>::value && !std::is_same<T, std::string>::value && !std::is_trivially_copyable<T>::value, MDBX_val>::type
    serialize_key(const T& value) {
        static_assert(sizeof(T) == 0, "Unsupported type for serialize_key");
        MDBX_val val = { 0, nullptr };
        return val;
    }

    // string
    template<typename T>
    typename std::enable_if<std::is_same<T, std::string>::value, MDBX_val>::type
    serialize_key(const T& key) {
        MDBX_val val;
        val.iov_base = const_cast<char*>(key.data());
        val.iov_len  = key.size();
        return val;
    }

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

    template<typename T>
    typename std::enable_if<
        std::is_same<T, int>::value || std::is_same<T, unsigned int>::value ||
        std::is_same<T, int32_t>::value || std::is_same<T, uint32_t>::value ||
        std::is_same<T, float>::value, MDBX_val>::type
    serialize_key(const T& key) {
        static_assert(sizeof(uint32_t) == 4, "Expected 4-byte integer");
        return MDBX_val{ sizeof(uint32_t), const_cast<void*>(static_cast<const void*>(&key)) };
    }

    template<typename T>
    typename std::enable_if<
        std::is_same<T, int64_t>::value ||
        std::is_same<T, uint64_t>::value ||
        std::is_same<T, double>::value, MDBX_val>::type
    serialize_key(const T& key) {
        static_assert(sizeof(uint64_t) == 8, "Expected 8-byte integer");
        return MDBX_val{ sizeof(uint64_t), const_cast<void*>(static_cast<const void*>(&key)) };
    }

    template<typename T>
    typename std::enable_if<
        std::is_same<T, std::byte>::value || std::is_same<T, uint8_t>::value ||
        std::is_same<T, int8_t>::value || std::is_same<T, char>::value ||
        std::is_same<T, unsigned char>::value ||
        std::is_same<T, int16_t>::value || std::is_same<T, uint16_t>::value,
        MDBX_val>::type
    serialize_key(const T& key) {
        static_assert(sizeof(uint32_t) == 4, "Expected 4-byte wrapper");
        thread_local uint32_t temp;
        temp = static_cast<uint32_t>(key);
        return MDBX_val{ sizeof(uint32_t), &temp };
    }

    // trivially copyable types (e.g., integers)
    template<typename T>
    typename std::enable_if<std::is_trivially_copyable<T>::value && !std::is_same<T, std::string>::value, MDBX_val>::type
    serialize_key(const T& key) {
        return MDBX_val{ sizeof(T), const_cast<void*>(static_cast<const void*>(&key)) };
    }

    template <size_t N>
    inline MDBX_val serialize_key(const std::bitset<N>& data) {
        const size_t num_bytes = (N + 7) / 8;
        static thread_local std::array<uint8_t, (N + 7) / 8> buffer;
        buffer.fill(0);
        for (size_t i = 0; i < N; ++i) {
            if (data[i]) buffer[i / 8] |= (1 << (i % 8));
        }
        return MDBX_val{ num_bytes, buffer.data() };
    }

    // --- serialize_value overloads ---

    /// \brief Serializes a general value into MDBX_val.
    /// \tparam T Type of the value.
    /// \param value The value to serialize.
    /// \return MDBX_val structure with binary representation of the value.
    template <typename T>
    typename std::enable_if<!has_to_bytes<T>::value && !std::is_trivially_copyable<T>::value, MDBX_val>::type
    serialize_value(const T& value) {
        static_assert(sizeof(T) == 0, "Unsupported type for serialize_value");
        MDBX_val val = { 0, nullptr };
        return val;
    }

    template<typename T>
    typename std::enable_if<std::is_same<T, std::string>::value, MDBX_val>::type
    serialize_value(const T& value) {
        return MDBX_val{ value.size(), const_cast<char*>(value.data()) };
    }

    template<typename T>
    typename std::enable_if<
        std::is_same<T, std::vector<std::byte>>::value ||
        std::is_same<T, std::vector<uint8_t>>::value ||
        std::is_same<T, std::vector<char>>::value ||
        std::is_same<T, std::vector<unsigned char>>::value, MDBX_val>::type
    serialize_value(const T& value) {
        return MDBX_val{ value.size(), const_cast<void*>(static_cast<const void*>(value.data())) };
    }
    
    template<typename T>
    typename std::enable_if<
        std::is_same<T, std::deque<std::byte>>::value ||
        std::is_same<T, std::deque<uint8_t>>::value ||
        std::is_same<T, std::deque<char>>::value ||
        std::is_same<T, std::deque<unsigned char>>::value,
        MDBX_val>::type
    serialize_value(const T& value) {
        static thread_local std::vector<uint8_t> buffer;
        buffer.assign(value.begin(), value.end());
        MDBX_val val;
        val.iov_base = static_cast<void*>(buffer.data());
        val.iov_len  = buffer.size();
        return val;
    }

    template<typename T>
    typename std::enable_if<
        std::is_same<T, std::list<std::byte>>::value ||
        std::is_same<T, std::list<uint8_t>>::value ||
        std::is_same<T, std::list<char>>::value ||
        std::is_same<T, std::list<unsigned char>>::value,
        MDBX_val>::type
    serialize_value(const T& value) {
        static thread_local std::vector<uint8_t> buffer;
        buffer.assign(value.begin(), value.end());
        MDBX_val val;
        val.iov_base = static_cast<void*>(buffer.data());
        val.iov_len  = buffer.size();
        return val;
    }

    template<typename T>
    typename std::enable_if<
        has_value_type<T>::value &&
        std::is_same<T, std::vector<typename T::value_type>>::value &&
        std::is_trivially_copyable<typename T::value_type>::value,
        MDBX_val>::type
    serialize_value(const T& value) {
        typedef typename T::value_type Elem;
        return MDBX_val{ value.size() * sizeof(Elem), const_cast<void*>(static_cast<const void*>(value.data())) };
    }

    template<typename T>
    typename std::enable_if<
        (std::is_same<T, std::deque<typename T::value_type>>::value ||
         std::is_same<T, std::list<typename T::value_type>>::value) &&
        std::is_trivially_copyable<typename T::value_type>::value,
        MDBX_val>::type
    serialize_value(const T& value) {
        typedef typename T::value_type Elem;
        static thread_local std::vector<Elem> buffer;
        buffer.assign(value.begin(), value.end());
        MDBX_val val;
        val.iov_base = static_cast<void*>(buffer.data());
        val.iov_len  = buffer.size() * sizeof(Elem);
        return val;
    }

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

    template<typename T>
    typename std::enable_if<std::is_trivially_copyable<T>::value, MDBX_val>::type
    serialize_value(const T& value) {
        MDBX_val val;
        val.iov_base = const_cast<void*>(static_cast<const void*>(&value));
        val.iov_len  = sizeof(T);
        return val;
    }

    // --- deserialize_value overloads ---
    
    /// \brief Deserializes a value from MDBX_val into type T.
    /// \tparam T Type of the value.
    /// \param val MDBX_val containing raw data.
    /// \return Deserialized T.
    template<typename T>
    typename std::enable_if<!has_from_bytes<T>::value && !std::is_trivially_copyable<T>::value, T>::type
    deserialize_value(const MDBX_val& val) {
        static_assert(sizeof(T) == 0, "Unsupported type for deserialize_value");
        T out;
        return out;
    }

    template<typename T>
    typename std::enable_if<std::is_same<T, std::string>::value, T>::type
    deserialize_value(const MDBX_val& val) {
        return std::string(static_cast<const char*>(val.iov_base), val.iov_len);
    }

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

    template<typename T>
    typename std::enable_if<std::is_trivially_copyable<T>::value, T>::type
    deserialize_value(const MDBX_val& val) {
        if (val.iov_len != sizeof(T))
            throw std::runtime_error("deserialize_value: size mismatch");
        T out;
        std::memcpy(&out, val.iov_base, sizeof(T));
        return out;
    }

    template<typename T>
    typename std::enable_if<has_from_bytes<T>::value, T>::type
    deserialize_value(const MDBX_val& val) {
        return T::from_bytes(val.iov_base, val.iov_len);
    }

}; // namespace mdbxc

#endif // _MDBX_CONTAINERS_UTILS_HPP_INCLUDED
