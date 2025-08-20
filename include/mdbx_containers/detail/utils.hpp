#pragma once
#ifndef _MDBX_CONTAINERS_UTILS_HPP_INCLUDED
#define _MDBX_CONTAINERS_UTILS_HPP_INCLUDED

/// \file utils.hpp
/// \brief Utility helper functions for serializing values to and from MDBX.
///        See: https://libmdbx.dqdkfa.ru/

/// \ingroup mdbxc_utils
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
    
    /// \brief Convert IEEE754 float to monotonic sortable unsigned int key.
    /// \param f Input float value.
    /// \return Unsigned 32-bit integer with preserved numeric order.
    inline uint32_t sortable_key_from_float(float f) {
        uint32_t u; 
        std::memcpy(&u, &f, sizeof(uint32_t));
        return (u & 0x80000000u) ? ~u : (u ^ 0x80000000u);
    }
    
    /// \brief Convert IEEE754 double to monotonic sortable unsigned int key.
    /// \param d Input double value.
    /// \return Unsigned 64-bit integer with preserved numeric order.
    inline uint64_t sortable_key_from_double(double d) {
        uint64_t u; 
        std::memcpy(&u, &d, sizeof(uint64_t));
        return (u & 0x8000000000000000ull) ? ~u : (u ^ 0x8000000000000000ull);
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
        if (
#           if __cplusplus >= 201703L
            std::is_same<T, std::vector<std::byte> >::value ||
#           endif
            std::is_same<T, std::vector<uint8_t> >::value ||
            std::is_same<T, std::vector<char> >::value ||
            std::is_same<T, std::vector<unsigned char> >::value) {
            return key.size();
        }

        // fallback
        return sizeof(T);
    }
    
    /// \class SerializeScratch
    /// \brief Per-call scratch buffer to produce \c MDBX_val without using \c thread_local.
    ///
    /// \details
    /// **Why not `thread_local`?** On Windows/MinGW, destructors of `thread_local`
    /// STL containers (e.g. `std::vector`) may run at thread teardown when parts of
    /// CRT/heap are already being finalized or when different heap arenas are involved.
    /// This can lead to heap corruption or `std::terminate()` without an exception.
    ///
    /// To avoid those issues, we keep a small stack-like inline buffer and a
    /// per-call dynamic buffer owned by the caller object (not `thread_local`).
    /// The lifetime of returned \c MDBX_val is guaranteed only until the next
    /// call that mutates this scratch, or until it goes out of scope.
    ///
    /// **Invariants & usage:**
    /// - `view(...)` does **not** copy; caller must ensure `(ptr,len)` lives through the MDBX call.
    /// - `view_small_copy(...)` copies up to 16 bytes into an aligned inline buffer.
    ///   It is intended for small keys (e.g., 4/8-byte INTEGERKEY).
    /// - `view_copy(...)` and `assign_bytes(...)` own copied data in `bytes`.
    /// - `view_bytes()` exposes the current contents of `bytes` without copying.
    /// - Do **not** store the returned `MDBX_val` beyond the scope of immediate MDBX API call.
    struct SerializeScratch {
        alignas(8) unsigned char small[16]; ///< Small inline buffer (16 bytes) aligned to 8 — good for 4/8-byte keys
        std::vector<uint8_t> bytes; ///< Owned dynamic buffer for cases when data must be copied

        /// \brief Zero-copy view over external memory (no ownership).
        /// \warning Caller must guarantee lifetime of \a p,\a n until MDBX finishes with it.
        [[nodiscard]] static inline MDBX_val view(const void* p, size_t n) noexcept {
            MDBX_val v;
            v.iov_base = (n ? const_cast<void*>(p) : nullptr);
            v.iov_len  = n;
            return v;
        }

        /// \brief Copy \a n bytes from \a p into \c bytes and return a view.
        [[nodiscard]] inline MDBX_val view_copy(const void* p, size_t n) {
            bytes.clear();
            bytes.resize(n);
            if (n) std::memcpy(bytes.data(), p, n);
            MDBX_val v;
            v.iov_base = (bytes.empty() ? nullptr : static_cast<void*>(bytes.data()));
            v.iov_len  = bytes.size();
            return v;
        }
        
        /// \brief Return a view over current \c bytes (no copy).
        [[nodiscard]] inline MDBX_val view_bytes() const noexcept {
            MDBX_val v;
            v.iov_base = (bytes.empty() ? nullptr : const_cast<void*>(static_cast<const void*>(bytes.data())));
            v.iov_len  = bytes.size();
            return v;
        }
        
        /// \brief Copy \a n bytes into the small inline buffer and return a view.
        /// \note \a n must be <= sizeof(small).
        [[nodiscard]] inline MDBX_val view_small_copy(const void* p, size_t n) noexcept {
            std::memcpy(small, p, n);
            MDBX_val v; 
            v.iov_base = (n ? static_cast<void*>(small) : nullptr); 
            v.iov_len = n; 
            return v;
        }

        /// \brief Replace \c bytes content with a copy of \a p..p+n .
        inline void assign_bytes(const void* p, size_t n) {
            bytes.clear();
            bytes.resize(n);
            if (n) std::memcpy(bytes.data(), p, n);
        }

        /// \brief Optionally clear and release capacity.
        inline void clear() noexcept { bytes.clear(); bytes.shrink_to_fit(); }
    };

    // --- serialize_key overloads ---


    /// \brief Serializes a key into MDBX_val for database operations.
    /// \tparam T Key type.
    /// \param key The key to convert.
    /// \return MDBX_val representing the key.
    template <typename T>
    typename std::enable_if<!has_to_bytes<T>::value && !std::is_same<T, std::string>::value && !std::is_trivially_copyable<T>::value, MDBX_val>::type
    serialize_key(const T& key, SerializeScratch& sc) {
        (void)key; 
        (void)sc;
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
    serialize_key(const T& key, SerializeScratch& sc) {
        (void)sc;
        return SerializeScratch::view(static_cast<const void*>(key.data()), key.size());
    }

    /// \brief Serializes a key stored in a byte vector.
    /// \tparam T Vector type containing bytes.
    template<typename T>
    typename std::enable_if<
#       if __cplusplus >= 201703L
        std::is_same<T, std::vector<std::byte>>::value ||
#       endif
        std::is_same<T, std::vector<uint8_t>>::value ||
        std::is_same<T, std::vector<char>>::value ||
        std::is_same<T, std::vector<unsigned char>>::value, MDBX_val>::type
    serialize_key(const T& key, SerializeScratch& sc) {
        (void)sc;
        return SerializeScratch::view(static_cast<const void*>(key.data()), key.size());
    }
    
    /// \brief Serializes a small integral key (<=16 bits).
    /// \tparam T Integral type.
    template<typename T>
    typename std::enable_if<
        std::is_integral<T>::value &&
        (sizeof(T) <= 2), MDBX_val>::type
    serialize_key(const T& key, SerializeScratch& sc) {
        static_assert(sizeof(uint32_t) == 4, "Expected 4-byte wrapper");
        uint32_t temp = static_cast<uint32_t>(key);
        return sc.view_small_copy(&temp, sizeof(uint32_t));
    }

    /// \brief Serializes a 32-bit integral key.
    /// \tparam T Supported 32-bit type.
    template<typename T>
    typename std::enable_if<
        std::is_same<T, int32_t>::value ||
        std::is_same<T, uint32_t>::value, MDBX_val>::type
    serialize_key(const T& key, SerializeScratch& sc) {
        static_assert(sizeof(uint32_t) == 4, "Expected 4-byte integer");
#       if MDBXC_SAFE_INTEGERKEY
        return sc.view_small_copy(&key, sizeof(uint32_t));
#       else
        (void)sc;
        return SerializeScratch::view(static_cast<const void*>(&key), sizeof(uint32_t));
#       endif
    }
    
    /// \brief Serializes a 32-bit float key.
    /// \tparam T Supported 32-bit type.
    template<typename T>
    typename std::enable_if<
        std::is_same<T, float>::value, MDBX_val>::type
    serialize_key(const T& key, SerializeScratch& sc) {
        static_assert(sizeof(uint32_t) == 4, "Expected 4-byte integer");
        uint32_t temp = sortable_key_from_float(key);
        return sc.view_small_copy(&temp, sizeof(uint32_t));
    }

    /// \brief Serializes a 64-bit integral key.
    /// \tparam T Supported 64-bit type.
    template<typename T>
    typename std::enable_if<
        std::is_same<T, int64_t>::value ||
        std::is_same<T, uint64_t>::value, MDBX_val>::type
    serialize_key(const T& key, SerializeScratch& sc) {
        static_assert(sizeof(uint64_t) == 8, "Expected 8-byte integer");
#       if MDBXC_SAFE_INTEGERKEY
        return sc.view_small_copy(&key, sizeof(uint64_t));
#       else
        (void)sc;
        return SerializeScratch::view(static_cast<const void*>(&key), sizeof(uint64_t));
#       endif
    }
    
    /// \brief Serializes a 64-bit double key.
    /// \tparam T Supported 64-bit type.
    template<typename T>
    typename std::enable_if<
        std::is_same<T, double>::value, MDBX_val>::type
    serialize_key(const T& key, SerializeScratch& sc) {
        static_assert(sizeof(uint64_t) == 8, "Expected 8-byte integer");
        uint64_t temp = sortable_key_from_double(key);
        return sc.view_small_copy(&temp, sizeof(uint64_t));
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
    serialize_key(const T& key, SerializeScratch& sc) {
        (void)sc;
        return SerializeScratch::view(static_cast<const void*>(&key), sizeof(T));
    }

    /// \brief Serializes a std::bitset as a key.
    /// \tparam N Number of bits in the bitset.
    /// \param data Bitset value to serialize.
    template <size_t N>
    inline MDBX_val serialize_key(const std::bitset<N>& data, SerializeScratch& sc) {
        const size_t num_bytes = (N + 7) / 8;
        std::array<uint8_t, (N + 7) / 8> buffer;
        buffer.fill(0);
        for (size_t i = 0; i < N; ++i) {
            if (data[i]) buffer[i / 8] |= (1 << (i % 8));
        }
        return sc.view_copy(buffer.data(), num_bytes);
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
    serialize_value(const T& value, SerializeScratch& sc) {
        (void)value; 
        (void)sc;
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
    serialize_value(const T& value, SerializeScratch& sc) {
        (void)sc;
        return SerializeScratch::view(static_cast<const void*>(value.data()), value.size());
    }
    
    /// \brief Serializes containers (vector, deque, list, set) of trivially copyable elements.
    /// \tparam T Container type with value_type.
    /// \param container The container to serialize.
    template <typename T>
    typename std::enable_if<
        has_value_type<T>::value &&
        std::is_trivially_copyable<typename T::value_type>::value &&
        (
            std::is_same<T, std::deque<typename T::value_type>>::value ||
            std::is_same<T, std::list<typename T::value_type>>::value ||
            std::is_same<T, std::set<typename T::value_type>>::value
        ),
        MDBX_val>::type
    serialize_value(const T& container, SerializeScratch& sc) {
        using Elem = typename T::value_type;
        sc.bytes.resize(container.size() * sizeof(Elem));
        auto* out = reinterpret_cast<Elem*>(sc.bytes.data());
        std::copy(container.begin(), container.end(), out);
        return sc.view_bytes();
    }

    /// \brief Serializes a vector of trivially copyable elements.
    /// \tparam T Vector type.
    template<typename T>
    typename std::enable_if<
        has_value_type<T>::value &&
        std::is_same<T, std::vector<typename T::value_type>>::value &&
        std::is_trivially_copyable<typename T::value_type>::value,
        MDBX_val>::type
    serialize_value(const T& container, SerializeScratch& sc) {
        using Elem = typename T::value_type;
        sc.bytes.resize(container.size() * sizeof(Elem));
        std::memcpy(sc.bytes.data(),
                    container.data(),
                    sc.bytes.size());   
        return sc.view_bytes();
    }

    /// \brief Serializes a value using its `to_bytes()` method.
    /// \tparam T Type providing `to_bytes`.
    template<typename T>
    typename std::enable_if<has_to_bytes<T>::value, MDBX_val>::type
    serialize_value(const T& value, SerializeScratch& sc) {
        sc.bytes = value.to_bytes();
        return sc.view_bytes();
    }

    /// \brief Serializes any trivially copyable value.
    /// \tparam T Trivially copyable type.
    template<typename T>
    typename std::enable_if<
        !has_to_bytes<T>::value &&
        std::is_trivially_copyable<T>::value,
        MDBX_val>::type
    serialize_value(const T& value, SerializeScratch& sc) {
        (void)sc;
        return SerializeScratch::view(static_cast<const void*>(&value), sizeof(T));
    }

    /// \brief Serializes a container of strings.
    /// \tparam T Container type with `std::string` elements.
    template<typename T>
    typename std::enable_if<
            has_value_type<T>::value &&
            std::is_same<typename T::value_type, std::string>::value,
            MDBX_val>::type
    serialize_value(const T& container, SerializeScratch& sc) {
        sc.bytes.clear();
        for (const auto& str : container) {
            uint32_t len = static_cast<uint32_t>(str.size());
            sc.bytes.insert(sc.bytes.end(),
                          reinterpret_cast<const uint8_t*>(&len),
                          reinterpret_cast<const uint8_t*>(&len) + sizeof(uint32_t));
            sc.bytes.insert(sc.bytes.end(), str.begin(), str.end());
        }
        return sc.view_bytes();
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
        (void)val;
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
#       if __cplusplus >= 201703L
        std::is_same<T, std::vector<std::byte>>::value ||
#       endif
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
#       if __cplusplus >= 201703L
        std::is_same<T, std::deque<std::byte>>::value ||
#       endif
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
#       if __cplusplus >= 201703L
        std::is_same<T, std::list<std::byte>>::value ||
#       endif
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
        if (val.iov_len % sizeof(Elem) != 0) {
            throw std::runtime_error("deserialize_value: size not aligned");
        }
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
        if (val.iov_len != sizeof(T)) {
            throw std::runtime_error("deserialize_value: size mismatch");
        }
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
