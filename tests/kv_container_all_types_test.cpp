#include <iostream>
#include <sstream>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <atomic>
#include <cassert>
#include <chrono>
#include <vector>
#include <deque>
#include <list>
#include <set>
#include <unordered_set>
#include <cstring>
#include <bitset>
#include <utility>
#include <stdexcept>

#include <mdbx_containers/KeyValueTable.hpp>

#if __cplusplus >= 201703L
#define ASSERT_FOUND(table, key, expected)                                                \
    do {                                                                                  \
        auto res = (table).find(key);                                                     \
        if (!res.has_value() || !(res.value() == (expected))) {                           \
            throw std::runtime_error("ASSERT_FOUND failed");                             \
        }                                                                                 \
    } while (0)
#else
#define ASSERT_FOUND(table, key, expected)                                                 \
    do {                                                                                  \
        auto res = (table).find_compat(key);                                              \
        if (!res.first || !(res.second == (expected))) {                                  \
            throw std::runtime_error("ASSERT_FOUND failed");                             \
        }                                                                                 \
    } while (0)
#endif

// ---- synchronized ostream (flushes once on scope exit) ----
class SyncOStream {
public:
    explicit SyncOStream(std::ostream& os) : os_(os) {}
    
    SyncOStream(const SyncOStream&) = delete;
    SyncOStream& operator=(const SyncOStream&) = delete;
    
    SyncOStream(SyncOStream&& other) noexcept
        : os_(other.os_), buffer_(std::move(other.buffer_)) {}
        
    SyncOStream& operator=(SyncOStream&&) = delete;

    template <typename T>
    SyncOStream& operator<<(const T& value) {
        buffer_ << value;
        return *this;
    }
    // manipulators (std::endl, etc.)
    SyncOStream& operator<<(std::ostream& (*manip)(std::ostream&)) {
        buffer_ << manip;
        return *this;
    }
    ~SyncOStream() {
        std::lock_guard<std::mutex> lock(get_mutex());
        os_ << buffer_.str();
        os_.flush();
    }
private:
    std::ostream& os_;
    std::ostringstream buffer_;
    static std::mutex& get_mutex() {
        static std::mutex m;
        return m;
    }
};

inline SyncOStream sync_cout() { return SyncOStream(std::cout); }
inline SyncOStream sync_cerr() { return SyncOStream(std::cerr); }

// ---- thread runner that captures exceptions ----
template <class F>
std::thread make_thread_catching(F&& f, std::exception_ptr& out_eptr) {
    typedef typename std::decay<F>::type Fn;
    std::shared_ptr<Fn> fn(new Fn(std::forward<F>(f)));
    return std::thread([fn, &out_eptr]{
        try { (*fn)(); }
        catch (...) { out_eptr = std::current_exception(); }
    });
}

// ---- safe join (no throw) ----
inline void safe_join(std::thread& t) noexcept {
    if (t.joinable()) t.join();
}

// ---- sample serializable structs ----
struct SimpleStruct {
    int   x{};
    float y{};

    SimpleStruct() = default;
    SimpleStruct(int x_, float y_) : x(x_), y(y_) {}

    std::vector<uint8_t> to_bytes() const {
        std::vector<uint8_t> bytes(sizeof(int) + sizeof(float));
        std::memcpy(bytes.data(), &x, sizeof(int));
        std::memcpy(bytes.data() + sizeof(int), &y, sizeof(float));
        return bytes;
    }
    static SimpleStruct from_bytes(const void* data, size_t size) {
        if (size != (sizeof(int) + sizeof(float)))
            throw std::runtime_error("Invalid data size for SimpleStruct");
        const uint8_t* ptr = static_cast<const uint8_t*>(data);
        SimpleStruct s{};
        std::memcpy(&s.x, ptr, sizeof(int));
        std::memcpy(&s.y, ptr + sizeof(int), sizeof(float));
        return s;
    }
    bool operator==(const SimpleStruct& other) const {
        return x == other.x && y == other.y;
    }
};

struct ConcurrentStruct {
    int value{};

    ConcurrentStruct() = default;
    explicit ConcurrentStruct(int v) : value(v) {}

    std::vector<uint8_t> to_bytes() const {
        std::vector<uint8_t> bytes(sizeof(int));
        std::memcpy(bytes.data(), &value, sizeof(int));
        return bytes;
    }
    static ConcurrentStruct from_bytes(const void* data, size_t size) {
        if (size != sizeof(int)) throw std::runtime_error("Invalid data size");
        ConcurrentStruct s{};
        std::memcpy(&s.value, data, sizeof(int));
        return s;
    }
    bool operator==(const ConcurrentStruct& other) const { return value == other.value; }
    bool operator!=(const ConcurrentStruct& other) const { return !(*this == other); }
};

int main() {
    mdbxc::Config cfg;
    cfg.pathname       = "data/kv_container_all_types";
    cfg.max_dbs        = 40;
    cfg.no_subdir      = false;
    cfg.relative_to_exe= true;

    auto conn = mdbxc::Connection::create(cfg);

    // --- basic cases ---
    std::cout << "[case] int8 -> int8\n";
    {
        mdbxc::KeyValueTable<int8_t, int8_t> kv(conn, "i8_i8");
        kv.insert_or_assign(1, 100);
        ASSERT_FOUND(kv, 1, 100);
    }

    std::cout << "[case] int8 -> int64\n";
    {
        mdbxc::KeyValueTable<int8_t, int64_t> kv(conn, "i8_i64");
        kv.insert_or_assign(2, 1234567890123456LL);
        ASSERT_FOUND(kv, 2, 1234567890123456LL);
    }

    std::cout << "[case] int32 -> string\n";
    {
        mdbxc::KeyValueTable<int32_t, std::string> kv(conn, "i32_str");
        kv.insert_or_assign(3, "hello");
        ASSERT_FOUND(kv, 3, std::string("hello"));
    }

    std::cout << "[case] safe/fast int32 key options compatibility\n";
    {
        mdbxc::KeyValueTable<int32_t, std::string> safe_kv(conn, "i32_options");
        mdbxc::KeyValueTable<int32_t, std::string, mdbxc::FastIntegerKeyOptions> fast_kv(conn, "i32_options");
        safe_kv.insert_or_assign(31, "safe");
        ASSERT_FOUND(fast_kv, 31, std::string("safe"));
        fast_kv.insert_or_assign(32, "fast");
        ASSERT_FOUND(safe_kv, 32, std::string("fast"));
    }

    std::cout << "[case] safe/fast uint64 key options compatibility\n";
    {
        mdbxc::KeyValueTable<uint64_t, std::string> safe_kv(conn, "u64_options");
        mdbxc::KeyValueTable<uint64_t, std::string, mdbxc::FastIntegerKeyOptions> fast_kv(conn, "u64_options");
        safe_kv.insert_or_assign(6401u, "safe");
        ASSERT_FOUND(fast_kv, 6401u, std::string("safe"));
        fast_kv.insert_or_assign(6402u, "fast");
        ASSERT_FOUND(safe_kv, 6402u, std::string("fast"));
    }

    std::cout << "[case] string -> string\n";
    {
        mdbxc::KeyValueTable<std::string, std::string> kv(conn, "str_str");
        kv.insert_or_assign("key", "value");
        ASSERT_FOUND(kv, std::string("key"), std::string("value"));
    }

    std::cout << "[case] pair insert_or_assign overloads\n";
    {
        mdbxc::KeyValueTable<int, std::string> kv(conn, "pair_insert_or_assign");

        std::pair<int, std::string> first = std::make_pair(1, std::string("one"));
        kv.insert_or_assign(first);
        ASSERT_FOUND(kv, 1, std::string("one"));

        std::pair<int, std::string> replacement = std::make_pair(1, std::string("uno"));
        kv.insert_or_assign(replacement);
        ASSERT_FOUND(kv, 1, std::string("uno"));

        mdbxc::Transaction txn = conn->transaction(mdbxc::TransactionMode::WRITABLE);
        std::pair<int, std::string> second = std::make_pair(2, std::string("two"));
        kv.insert_or_assign(second, txn);
        txn.commit();
        ASSERT_FOUND(kv, 2, std::string("two"));
    }

    std::cout << "[case] key range values\n";
    {
        mdbxc::KeyValueTable<int, std::string> kv(conn, "range_values");
        kv.clear();

        kv.insert_or_assign(1, "one");
        kv.insert_or_assign(2, "two");
        kv.insert_or_assign(4, "four");
        kv.insert_or_assign(5, "five");

        assert(kv.range(2, 4) == (std::vector<std::string>{"two", "four"}));
        assert(kv.range(0, 1) == (std::vector<std::string>{"one"}));
        assert(kv.range(3, 3).empty());
        assert(kv.range(5, 2).empty());

        mdbxc::Transaction read_txn = conn->transaction(mdbxc::TransactionMode::READ_ONLY);
        assert(kv.range(1, 2, read_txn) == (std::vector<std::string>{"one", "two"}));
        read_txn.commit();
    }

    std::cout << "[case] string -> POD(SimpleStruct)\n";
    {
        mdbxc::KeyValueTable<std::string, SimpleStruct> kv(conn, "str_struct");
        SimpleStruct s{42, 3.14f};
        kv.insert_or_assign("obj", s);
        ASSERT_FOUND(kv, std::string("obj"), s);
    }

    std::cout << "[case] string -> set<int>\n";
    {
        mdbxc::KeyValueTable<std::string, std::set<int>> kv(conn, "str_set_int");
        std::set<int> s{1, 2, 3};
        kv.insert_or_assign("digits", s);
        ASSERT_FOUND(kv, std::string("digits"), s);
    }

    std::cout << "[case] string -> unordered_set<int>\n";
    {
        mdbxc::KeyValueTable<std::string, std::unordered_set<int>> kv(conn, "str_unordered_set_int");
        std::unordered_set<int> s{1, 2, 3};
        kv.insert_or_assign("digits", s);
        ASSERT_FOUND(kv, std::string("digits"), s);
    }

    std::cout << "[case] string -> deque<int>\n";
    {
        mdbxc::KeyValueTable<std::string, std::deque<int>> kv(conn, "str_deque_int");
        std::deque<int> values{1, 2, 3};
        kv.insert_or_assign("digits", values);
        ASSERT_FOUND(kv, std::string("digits"), values);
    }

    std::cout << "[case] string -> list<int>\n";
    {
        mdbxc::KeyValueTable<std::string, std::list<int>> kv(conn, "str_list_int");
        std::list<int> values{1, 2, 3};
        kv.insert_or_assign("digits", values);
        ASSERT_FOUND(kv, std::string("digits"), values);
    }

    std::cout << "[case] int64 -> vector<uint8_t>\n";
    {
        mdbxc::KeyValueTable<int64_t, std::vector<uint8_t>> kv(conn, "i64_vec");
        std::vector<uint8_t> data{1, 2, 3, 4};
        kv.insert_or_assign(9, data);
        ASSERT_FOUND(kv, 9, data);
    }

    std::cout << "[case] string -> vector<SimpleStruct>\n";
    {
        mdbxc::KeyValueTable<std::string, std::vector<SimpleStruct>> kv(conn, "str_vec_struct");
        std::vector<SimpleStruct> vec{{1, 1.0f}, {2, 2.0f}};
        kv.insert_or_assign("many", vec);
        ASSERT_FOUND(kv, std::string("many"), vec);
    }

    std::cout << "[case] string -> list<string>\n";
    {
        mdbxc::KeyValueTable<std::string, std::list<std::string>> kv(conn, "str_list_str");
        std::list<std::string> lst{"a", "b", "c"};
        kv.insert_or_assign("letters", lst);
        ASSERT_FOUND(kv, std::string("letters"), lst);
    }

    std::cout << "[case] string -> deque<string>\n";
    {
        mdbxc::KeyValueTable<std::string, std::deque<std::string>> kv(conn, "str_deque_str");
        std::deque<std::string> values{"a", "b", "c"};
        kv.insert_or_assign("letters", values);
        ASSERT_FOUND(kv, std::string("letters"), values);
    }

    std::cout << "[case] string -> vector<string>\n";
    {
        mdbxc::KeyValueTable<std::string, std::vector<std::string>> kv(conn, "str_vector_str");
        std::vector<std::string> lst{"a", "b", "c"};
        kv.insert_or_assign("letters", lst);
        ASSERT_FOUND(kv, std::string("letters"), lst);
    }

    std::cout << "[case] string -> set<string>\n";
    {
        mdbxc::KeyValueTable<std::string, std::set<std::string>> kv(conn, "str_set_str");
        std::set<std::string> s{"a", "b", "c"};
        kv.insert_or_assign("letters", s);
        ASSERT_FOUND(kv, std::string("letters"), s);
    }

    std::cout << "[case] string -> unordered_set<string>\n";
    {
        mdbxc::KeyValueTable<std::string, std::unordered_set<std::string>> kv(conn, "str_unordered_set_str");
        std::unordered_set<std::string> s{"a", "b", "c"};
        kv.insert_or_assign("letters", s);
        ASSERT_FOUND(kv, std::string("letters"), s);
    }

    std::cout << "[case] string -> self-serializable struct\n";
    {
        struct Serializable {
            int a = 0;
            std::string b;

            Serializable() {}
            Serializable(int a_, const std::string& b_) : a(a_), b(b_) {}

            std::vector<uint8_t> to_bytes() const {
                std::vector<uint8_t> result(sizeof(int) + b.size());
                std::memcpy(result.data(), &a, sizeof(int));
                std::memcpy(result.data() + sizeof(int), b.data(), b.size());
                return result;
            }
            static Serializable from_bytes(const void* data, size_t size) {
                if (size < sizeof(int))
                    throw std::runtime_error("Invalid data size for Serializable");
                const uint8_t* ptr = static_cast<const uint8_t*>(data);
                Serializable s;
                std::memcpy(&s.a, ptr, sizeof(int));
                s.b = std::string(reinterpret_cast<const char*>(ptr + sizeof(int)), size - sizeof(int));
                return s;
            }
            bool operator==(const Serializable& other) const {
                return a == other.a && b == other.b;
            }
        };

        mdbxc::KeyValueTable<std::string, Serializable> kv(conn, "str_serializable");
        Serializable s{7, "seven"};
        kv.insert_or_assign("ser", s);
        ASSERT_FOUND(kv, std::string("ser"), s);
    }

    // --- NEW: bitset key example ---
    std::cout << "[case] std::bitset<32> -> int\n";
    {
        using Bits = std::bitset<32>;
        mdbxc::KeyValueTable<Bits, int> kv(conn, "bitset32_int");
        Bits key(std::string("10101010101010101010101010101010")); // 32-bit pattern
        kv.insert_or_assign(key, 31415);
        ASSERT_FOUND(kv, key, 31415);
    }

    // --- concurrency smoke test ---
    std::cout << "[concurrency] start\n";
    {
        mdbxc::KeyValueTable<int, ConcurrentStruct> kv(conn, "concurrent_test");

        std::mutex mtx;
        std::condition_variable cv;
        std::size_t epoch = 0;
        std::size_t last_seen = 0;
        std::atomic<bool> failed(false);
        std::atomic<bool> done(false);
        ConcurrentStruct written;
        std::exception_ptr w_ex, r_ex;

        auto writer = make_thread_catching([&kv, &failed, &mtx, &written, &epoch, &cv, &done] {
            try {
                for (int i = 0; i < 1000; ++i) {
                    if (failed) break;
                    ConcurrentStruct w(i);
                    {
                        std::lock_guard<std::mutex> lock(mtx);
                        kv.insert_or_assign(1, w);
                        written = w;
                        ++epoch;
                    }
                    cv.notify_one();
                    std::this_thread::sleep_for(std::chrono::milliseconds(1));
                }
                done.store(true, std::memory_order_release);
                cv.notify_all();
                sync_cout() << "[writer] done\n";
            } catch (const std::exception& ex) {
                sync_cerr() << "[error] writer: " << ex.what() << "\n";
                failed = true;
                done.store(true, std::memory_order_release);
                cv.notify_all();
            } catch (...) {
                sync_cerr() << "[error] writer: unknown\n";
                failed = true;
                done.store(true, std::memory_order_release);
                cv.notify_all();
            }
        }, w_ex);

        auto reader = make_thread_catching([&mtx, &cv, &epoch, &last_seen, &done, &failed, &written, &kv] {
            try {
                for (;;) {
                    std::unique_lock<std::mutex> lock(mtx);
                    bool ok = cv.wait_for(lock, std::chrono::seconds(2), [&epoch, &last_seen, &done] {
                        return epoch > last_seen || done.load(std::memory_order_acquire);
                    });
                    if (!ok) {
                        sync_cerr() << "[error] reader timeout (epoch=" << epoch
                                    << ", last_seen=" << last_seen << ")\n";
                        failed = true;
                        break;
                    }
                    if (done.load(std::memory_order_acquire) && epoch == last_seen) {
                        break;
                    }
                    last_seen = epoch;
                    ConcurrentStruct expected = written;
                    lock.unlock();

#                   if __cplusplus >= 201703L
                    auto val = kv.find(1);
                    if (!val || *val != expected) {
                        sync_cerr() << "[error] mismatch: got "
                                    << (val ? val->value : -1)
                                    << ", expected " << expected.value << "\n";
                        failed = true;
                        break;
                    }
#                   else
                    auto val = kv.find_compat(1);
                    if (!val.first || val.second != expected) {
                        sync_cerr() << "[error] mismatch: got "
                                    << val.second.value
                                    << ", expected " << expected.value << "\n";
                        failed = true;
                        break;
                    }
#                   endif
                }
                sync_cout() << "[reader] done\n";
            } catch (const std::exception& ex) {
                sync_cerr() << "[error] reader: " << ex.what() << "\n";
                failed = true;
            } catch (...) {
                sync_cerr() << "[error] reader: unknown\n";
                failed = true;
            }
        }, r_ex);

        safe_join(writer);
        safe_join(reader);

        if (w_ex) {
            try { std::rethrow_exception(w_ex); }
            catch (const std::exception& e) { sync_cerr() << "[error] writer rethrow: " << e.what() << "\n"; }
            catch (...) { sync_cerr() << "[error] writer rethrow: unknown\n"; }
        }
        if (r_ex) {
            try { std::rethrow_exception(r_ex); }
            catch (const std::exception& e) { sync_cerr() << "[error] reader rethrow: " << e.what() << "\n"; }
            catch (...) { sync_cerr() << "[error] reader rethrow: unknown\n"; }
        }

        if (w_ex || r_ex) {
            sync_cerr() << "[concurrency] failed: thread exception\n";
            return 1;
        }
        if (failed) {
            std::cerr << "[concurrency] failed\n";
            return 1;
        }

        std::cout << "[concurrency] ok\n";
    }

    std::cout << "[result] all tests passed\n";
    return 0;
}
