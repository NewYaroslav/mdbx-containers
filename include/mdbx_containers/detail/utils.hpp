#pragma once
#ifndef MDBX_CONTAINERS_HEADER_DETAIL_UTILS_HPP_INCLUDED
#define MDBX_CONTAINERS_HEADER_DETAIL_UTILS_HPP_INCLUDED

/// \file utils.hpp
/// \brief Utility helper functions for serializing values to and from MDBX.
///        See: https://libmdbx.dqdkfa.ru/

/// \ingroup mdbxc_utils
/// @{

#include <bitset>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <deque>
#include <list>

#include <mdbx.h>
#include <map>
#include <memory>
#include <set>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "../common/MdbxException.hpp"

#if __cplusplus >= 201703L
#   define MDBXC_NODISCARD [[nodiscard]]
#else
#   define MDBXC_NODISCARD
#endif

namespace mdbxc {
    
    /// \brief Throws an MdbxException if MDBX return code indicates an error.
    /// \param rc      Return code from an MDBX function.
    /// \param context Description of the calling context.
    inline void check_mdbx(int rc, const std::string& context) {
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

    template <typename T>
    struct is_string_sequence_container {
        static const bool value =
            std::is_same<T, std::vector<std::string>>::value ||
            std::is_same<T, std::deque<std::string>>::value ||
            std::is_same<T, std::list<std::string>>::value;
    };

    template <typename T>
    struct is_string_set_container {
        static const bool value =
            std::is_same<T, std::set<std::string>>::value ||
            std::is_same<T, std::unordered_set<std::string>>::value;
    };

    /// \brief Compile-time policy options for table behavior.
    /// \tparam SafeIntegerKey Copy 32/64-bit integer keys into aligned scratch
    ///         storage before MDBX calls when true.
    template<bool SafeIntegerKey = true>
    struct TableOptions {
        static const bool safe_integer_key = SafeIntegerKey;
    };

    /// \brief Default table policy using safe integer key serialization.
    typedef TableOptions<true> DefaultTableOptions;

    /// \brief Table policy using direct views for 32/64-bit integer keys.
    ///
    /// \warning This mode is a lifetime/alignment tradeoff and does not change
    ///          the database storage format.
    typedef TableOptions<false> FastIntegerKeyOptions;

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
        MDBXC_NODISCARD static inline MDBX_val view(const void* p, size_t n) noexcept {
            MDBX_val v;
            v.iov_base = (n ? const_cast<void*>(p) : nullptr);
            v.iov_len  = n;
            return v;
        }

        /// \brief Copy \a n bytes from \a p into \c bytes and return a view.
        MDBXC_NODISCARD inline MDBX_val view_copy(const void* p, size_t n) {
            bytes.clear();
            bytes.resize(n);
            if (n) std::memcpy(bytes.data(), p, n);
            MDBX_val v;
            v.iov_base = (bytes.empty() ? nullptr : static_cast<void*>(bytes.data()));
            v.iov_len  = bytes.size();
            return v;
        }
        
        /// \brief Return a view over current \c bytes (no copy).
        MDBXC_NODISCARD inline MDBX_val view_bytes() const noexcept {
            MDBX_val v;
            v.iov_base = (bytes.empty() ? nullptr : const_cast<void*>(static_cast<const void*>(bytes.data())));
            v.iov_len  = bytes.size();
            return v;
        }
        
        /// \brief Copy \a n bytes into the small inline buffer and return a view.
        /// \note \a n must be <= sizeof(small).
        MDBXC_NODISCARD inline MDBX_val view_small_copy(const void* p, size_t n) noexcept {
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
    template <bool SafeIntegerKey = true, typename T>
    typename std::enable_if<!has_to_bytes<T>::value && !std::is_same<T, std::string>::value && !std::is_trivially_copyable<T>::value, MDBX_val>::type
    serialize_key(const T& key, SerializeScratch& sc) {
        (void)SafeIntegerKey;
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
    template<bool SafeIntegerKey = true, typename T>
    typename std::enable_if<std::is_same<T, std::string>::value, MDBX_val>::type
    serialize_key(const T& key, SerializeScratch& sc) {
        (void)SafeIntegerKey;
        (void)sc;
        return SerializeScratch::view(static_cast<const void*>(key.data()), key.size());
    }

    /// \brief Serializes a key stored in a byte vector.
    /// \tparam T Vector type containing bytes.
    template<bool SafeIntegerKey = true, typename T>
    typename std::enable_if<
#       if __cplusplus >= 201703L
        std::is_same<T, std::vector<std::byte>>::value ||
#       endif
        std::is_same<T, std::vector<uint8_t>>::value ||
        std::is_same<T, std::vector<char>>::value ||
        std::is_same<T, std::vector<unsigned char>>::value, MDBX_val>::type
    serialize_key(const T& key, SerializeScratch& sc) {
        (void)SafeIntegerKey;
        (void)sc;
        return SerializeScratch::view(static_cast<const void*>(key.data()), key.size());
    }
    
    /// \brief Serializes a small integral key (<=16 bits).
    /// \tparam T Integral type.
    template<bool SafeIntegerKey = true, typename T>
    typename std::enable_if<
        std::is_integral<T>::value &&
        (sizeof(T) <= 2), MDBX_val>::type
    serialize_key(const T& key, SerializeScratch& sc) {
        (void)SafeIntegerKey;
        static_assert(sizeof(uint32_t) == 4, "Expected 4-byte wrapper");
        uint32_t temp = static_cast<uint32_t>(key);
        return sc.view_small_copy(&temp, sizeof(uint32_t));
    }

    /// \brief Serializes a 32-bit integral key.
    /// \tparam T Supported 32-bit type.
    template<bool SafeIntegerKey = true, typename T>
    typename std::enable_if<
        std::is_same<T, int32_t>::value ||
        std::is_same<T, uint32_t>::value, MDBX_val>::type
    serialize_key(const T& key, SerializeScratch& sc) {
        static_assert(sizeof(uint32_t) == 4, "Expected 4-byte integer");
        return SafeIntegerKey
            ? sc.view_small_copy(&key, sizeof(uint32_t))
            : SerializeScratch::view(static_cast<const void*>(&key), sizeof(uint32_t));
    }
    
    /// \brief Serializes a 32-bit float key.
    /// \tparam T Supported 32-bit type.
    template<bool SafeIntegerKey = true, typename T>
    typename std::enable_if<
        std::is_same<T, float>::value, MDBX_val>::type
    serialize_key(const T& key, SerializeScratch& sc) {
        (void)SafeIntegerKey;
        static_assert(sizeof(uint32_t) == 4, "Expected 4-byte integer");
        uint32_t temp = sortable_key_from_float(key);
        return sc.view_small_copy(&temp, sizeof(uint32_t));
    }

    /// \brief Serializes a 64-bit integral key.
    /// \tparam T Supported 64-bit type.
    template<bool SafeIntegerKey = true, typename T>
    typename std::enable_if<
        std::is_same<T, int64_t>::value ||
        std::is_same<T, uint64_t>::value, MDBX_val>::type
    serialize_key(const T& key, SerializeScratch& sc) {
        static_assert(sizeof(uint64_t) == 8, "Expected 8-byte integer");
        return SafeIntegerKey
            ? sc.view_small_copy(&key, sizeof(uint64_t))
            : SerializeScratch::view(static_cast<const void*>(&key), sizeof(uint64_t));
    }
    
    /// \brief Serializes a 64-bit double key.
    /// \tparam T Supported 64-bit type.
    template<bool SafeIntegerKey = true, typename T>
    typename std::enable_if<
        std::is_same<T, double>::value, MDBX_val>::type
    serialize_key(const T& key, SerializeScratch& sc) {
        (void)SafeIntegerKey;
        static_assert(sizeof(uint64_t) == 8, "Expected 8-byte integer");
        uint64_t temp = sortable_key_from_double(key);
        return sc.view_small_copy(&temp, sizeof(uint64_t));
    }

    /// \brief Serializes any other trivially copyable key type.
    /// \tparam T Trivially copyable type.
    template<bool SafeIntegerKey = true, typename T>
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
        (void)SafeIntegerKey;
        (void)sc;
        return SerializeScratch::view(static_cast<const void*>(&key), sizeof(T));
    }

    /// \brief Serializes a std::bitset as a key.
    /// \tparam N Number of bits in the bitset.
    /// \param data Bitset value to serialize.
    template <bool SafeIntegerKey = true, size_t N>
    inline MDBX_val serialize_key(const std::bitset<N>& data, SerializeScratch& sc) {
        (void)SafeIntegerKey;
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
    
    /// \brief Serializes containers (vector, deque, list, set, unordered_set) of trivially copyable elements.
    /// \tparam T Container type with value_type.
    /// \param container The container to serialize.
    template <typename T>
    typename std::enable_if<
        has_value_type<T>::value &&
        std::is_trivially_copyable<typename T::value_type>::value &&
        (
            std::is_same<T, std::deque<typename T::value_type>>::value ||
            std::is_same<T, std::list<typename T::value_type>>::value ||
            std::is_same<T, std::set<typename T::value_type>>::value ||
            std::is_same<T, std::unordered_set<typename T::value_type>>::value
        ),
        MDBX_val>::type
    serialize_value(const T& container, SerializeScratch& sc) {
        using Elem = typename T::value_type;
        sc.bytes.resize(container.size() * sizeof(Elem));
        size_t offset = 0;
        for (typename T::const_iterator it = container.begin(); it != container.end(); ++it) {
            std::memcpy(sc.bytes.data() + offset, &(*it), sizeof(Elem));
            offset += sizeof(Elem);
        }
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
        if (!sc.bytes.empty()) {
            std::memcpy(sc.bytes.data(),
                        container.data(),
                        sc.bytes.size());
        }
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

    /// \brief Serializes a sequence container of strings.
    /// \tparam T `std::vector`, `std::deque`, or `std::list` of `std::string`.
    template<typename T>
    typename std::enable_if<is_string_sequence_container<T>::value, MDBX_val>::type
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

    /// \brief Serializes a set-like container of strings.
    /// \tparam T `std::set<std::string>` or `std::unordered_set<std::string>`.
    template<typename T>
    typename std::enable_if<is_string_set_container<T>::value, MDBX_val>::type
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
        !has_value_type<T>::value &&
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
        if (val.iov_len == 0) {
            return T();
        }
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
        if (val.iov_len == 0) {
            return T();
        }
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
        if (val.iov_len == 0) {
            return T();
        }
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
        if (val.iov_len == 0) {
            return T();
        }
        const uint8_t* ptr = static_cast<const uint8_t*>(val.iov_base);
        return T(ptr, ptr + val.iov_len);
    }

    /// \brief Deserializes a vector of trivially copyable elements.
    /// \tparam T Vector type.
    template<typename T>
    typename std::enable_if<
        has_value_type<T>::value &&
        std::is_same<T, std::vector<typename T::value_type>>::value &&
        std::is_trivially_copyable<typename T::value_type>::value &&
#       if __cplusplus >= 201703L
        !std::is_same<T, std::vector<std::byte>>::value &&
#       endif
        !std::is_same<T, std::vector<uint8_t>>::value &&
        !std::is_same<T, std::vector<char>>::value &&
        !std::is_same<T, std::vector<unsigned char>>::value, T>::type
    deserialize_value(const MDBX_val& val) {
        typedef typename T::value_type Elem;
        if (val.iov_len % sizeof(Elem) != 0)
            throw std::runtime_error("deserialize_value: size not aligned");
        const size_t count = val.iov_len / sizeof(Elem);
        T out(count);
        if (count) {
            std::memcpy(out.data(), val.iov_base, val.iov_len);
        }
        return out;
    }

    /// \brief Deserializes a deque or list of trivially copyable elements.
    /// \tparam T Container type.
    template<typename T>
    typename std::enable_if<
        (std::is_same<T, std::deque<typename T::value_type>>::value ||
         std::is_same<T, std::list<typename T::value_type>>::value) &&
        std::is_trivially_copyable<typename T::value_type>::value &&
#       if __cplusplus >= 201703L
        !std::is_same<T, std::deque<std::byte>>::value &&
        !std::is_same<T, std::list<std::byte>>::value &&
#       endif
        !std::is_same<T, std::deque<uint8_t>>::value &&
        !std::is_same<T, std::deque<char>>::value &&
        !std::is_same<T, std::deque<unsigned char>>::value &&
        !std::is_same<T, std::list<uint8_t>>::value &&
        !std::is_same<T, std::list<char>>::value &&
        !std::is_same<T, std::list<unsigned char>>::value, T>::type
    deserialize_value(const MDBX_val& val) {
        typedef typename T::value_type Elem;
        if (val.iov_len % sizeof(Elem) != 0) {
            throw std::runtime_error("deserialize_value: size not aligned");
        }
        const size_t count = val.iov_len / sizeof(Elem);
        const uint8_t* data = static_cast<const uint8_t*>(val.iov_base);
        T out;
        for (size_t i = 0; i < count; ++i) {
            Elem elem;
            std::memcpy(&elem, data + i * sizeof(Elem), sizeof(Elem));
            out.push_back(elem);
        }
        return out;
    }

    /// \brief Deserializes a set of trivially copyable elements.
    /// \tparam T Set container type.
    template<typename T>
    typename std::enable_if<
        std::is_same<T, std::set<typename T::value_type>>::value &&
        std::is_trivially_copyable<typename T::value_type>::value,
        T>::type
    deserialize_value(const MDBX_val& val) {
        typedef typename T::value_type Elem;
        if (val.iov_len % sizeof(Elem) != 0)
            throw std::runtime_error("deserialize_value: size not aligned");
        const size_t count = val.iov_len / sizeof(Elem);
        const uint8_t* data = static_cast<const uint8_t*>(val.iov_base);
        T out;
        typename T::iterator hint = out.end();
        for (size_t i = 0; i < count; ++i) {
            Elem elem;
            std::memcpy(&elem, data + i * sizeof(Elem), sizeof(Elem));
            hint = out.insert(hint, elem);
        }
        return out;
    }

    /// \brief Deserializes an unordered_set of trivially copyable elements.
    /// \tparam T Unordered set container type.
    template<typename T>
    typename std::enable_if<
        std::is_same<T, std::unordered_set<typename T::value_type>>::value &&
        std::is_trivially_copyable<typename T::value_type>::value,
        T>::type
    deserialize_value(const MDBX_val& val) {
        typedef typename T::value_type Elem;
        if (val.iov_len % sizeof(Elem) != 0)
            throw std::runtime_error("deserialize_value: size not aligned");
        const size_t count = val.iov_len / sizeof(Elem);
        const uint8_t* data = static_cast<const uint8_t*>(val.iov_base);
        T out;
        out.reserve(count);
        for (size_t i = 0; i < count; ++i) {
            Elem elem;
            std::memcpy(&elem, data + i * sizeof(Elem), sizeof(Elem));
            out.insert(elem);
        }
        return out;
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

    /// \brief Deserializes a sequence container of strings.
    /// \tparam T `std::vector`, `std::deque`, or `std::list` of `std::string`.
    template<typename T>
    typename std::enable_if<is_string_sequence_container<T>::value, T>::type
    deserialize_value(const MDBX_val& val) {
        if (val.iov_len == 0) {
            return T();
        }
        const uint8_t* ptr = static_cast<const uint8_t*>(val.iov_base);
        const uint8_t* end = ptr + val.iov_len;

        T result;
        while (ptr + sizeof(uint32_t) <= end) {
            uint32_t len;
            std::memcpy(&len, ptr, sizeof(uint32_t));
            ptr += sizeof(uint32_t);

            const size_t remaining = static_cast<size_t>(end - ptr);
            if (len > remaining)
                throw std::runtime_error("deserialize_value: corrupted data (length overflow)");

            result.emplace_back(reinterpret_cast<const char*>(ptr), len);
            ptr += len;
        }

        if (ptr != end)
            throw std::runtime_error("deserialize_value: trailing data after deserialization");

        return result;
    }
    
    /// \brief Deserializes a set-like container of strings.
    /// \tparam T Either `std::set<std::string>` or `std::unordered_set<std::string>`.
    template<typename T>
    typename std::enable_if<is_string_set_container<T>::value, T>::type
    deserialize_value(const MDBX_val& val) {
        if (val.iov_len == 0) {
            return T();
        }
        const uint8_t* ptr = static_cast<const uint8_t*>(val.iov_base);
        const uint8_t* end = ptr + val.iov_len;
        T result;

        while (ptr + sizeof(uint32_t) <= end) {
            uint32_t len;
            std::memcpy(&len, ptr, sizeof(uint32_t));
            ptr += sizeof(uint32_t);

            const size_t remaining = static_cast<size_t>(end - ptr);
            if (len > remaining)
                throw std::runtime_error("deserialize_value: corrupted data (length overflow)");

            result.insert(std::string(reinterpret_cast<const char*>(ptr), len));
            ptr += len;
        }

        if (ptr != end)
            throw std::runtime_error("deserialize_value: trailing data after deserialization");

        return result;
    }

    template<typename T>
    typename std::enable_if<std::is_same<T, std::string>::value, T>::type
    deserialize_key(const MDBX_val& val) {
        return deserialize_value<T>(val);
    }

    template<typename T>
    typename std::enable_if<
#       if __cplusplus >= 201703L
        std::is_same<T, std::vector<std::byte>>::value ||
#       endif
        std::is_same<T, std::vector<uint8_t>>::value ||
        std::is_same<T, std::vector<char>>::value ||
        std::is_same<T, std::vector<unsigned char>>::value, T>::type
    deserialize_key(const MDBX_val& val) {
        return deserialize_value<T>(val);
    }

    template<typename T>
    typename std::enable_if<
        std::is_integral<T>::value &&
        (sizeof(T) <= 2), T>::type
    deserialize_key(const MDBX_val& val) {
        if (val.iov_len != sizeof(uint32_t)) {
            throw std::runtime_error("deserialize_key: size mismatch");
        }
        uint32_t out;
        std::memcpy(&out, val.iov_base, sizeof(uint32_t));
        return static_cast<T>(out);
    }

    template<typename T>
    typename std::enable_if<
        std::is_same<T, int32_t>::value ||
        std::is_same<T, uint32_t>::value ||
        std::is_same<T, int64_t>::value ||
        std::is_same<T, uint64_t>::value, T>::type
    deserialize_key(const MDBX_val& val) {
        return deserialize_value<T>(val);
    }

    inline float float_from_sortable_key(uint32_t key) {
        uint32_t bits = (key & 0x80000000u) ? (key ^ 0x80000000u) : ~key;
        float out;
        std::memcpy(&out, &bits, sizeof(float));
        return out;
    }

    inline double double_from_sortable_key(uint64_t key) {
        uint64_t bits = (key & 0x8000000000000000ull)
            ? (key ^ 0x8000000000000000ull)
            : ~key;
        double out;
        std::memcpy(&out, &bits, sizeof(double));
        return out;
    }

    template<typename T>
    typename std::enable_if<std::is_same<T, float>::value, T>::type
    deserialize_key(const MDBX_val& val) {
        if (val.iov_len != sizeof(uint32_t)) {
            throw std::runtime_error("deserialize_key: size mismatch");
        }
        uint32_t raw;
        std::memcpy(&raw, val.iov_base, sizeof(uint32_t));
        return float_from_sortable_key(raw);
    }

    template<typename T>
    typename std::enable_if<std::is_same<T, double>::value, T>::type
    deserialize_key(const MDBX_val& val) {
        if (val.iov_len != sizeof(uint64_t)) {
            throw std::runtime_error("deserialize_key: size mismatch");
        }
        uint64_t raw;
        std::memcpy(&raw, val.iov_base, sizeof(uint64_t));
        return double_from_sortable_key(raw);
    }

    template <size_t N>
    inline std::bitset<N> deserialize_key(const MDBX_val& val) {
        const size_t num_bytes = (N + 7) / 8;
        if (val.iov_len != num_bytes) {
            throw std::runtime_error("deserialize_key: size mismatch");
        }
        const uint8_t* bytes = static_cast<const uint8_t*>(val.iov_base);
        std::bitset<N> out;
        for (size_t i = 0; i < N; ++i) {
            if (bytes[i / 8] & (1u << (i % 8))) out.set(i);
        }
        return out;
    }

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
        !std::is_same<T, double>::value, T>::type
    deserialize_key(const MDBX_val& val) {
        return deserialize_value<T>(val);
    }

}; // namespace mdbxc

/// @}

#undef MDBXC_NODISCARD

#endif // MDBX_CONTAINERS_HEADER_DETAIL_UTILS_HPP_INCLUDED
