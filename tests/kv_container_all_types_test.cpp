#include <iostream>
#include <sstream>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <atomic>
#include <cassert>
#include <mdbx_containers/KeyValueTable.hpp>

#if __cplusplus >= 201703L
#define ASSERT_FOUND(table, key, expected) assert((table).find(key).value() == (expected))
#else
#define ASSERT_FOUND(table, key, expected)                                                 \
    do {                                                                                  \
        auto res = (table).find_compat(key);                                              \
        assert(res.first && res.second == (expected));                                    \
    } while (0)
#endif

class SyncOStream {
public:
    explicit SyncOStream(std::ostream& os) : os_(os) {}

    template <typename T>
    SyncOStream& operator<<(const T& value) {
        buffer_ << value;
        return *this;
    }

    // поддержка манипуляторов типа std::endl
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

// ---- обёртка запуска задачи с перехватом исключений ----
template <class F>
std::thread make_thread_catching(F&& f, std::exception_ptr& out_eptr) {
    return std::thread([fn = std::forward<F>(f), &out_eptr] {
        try {
            fn();
        } catch (...) {
            out_eptr = std::current_exception(); // переносим наверх
        }
    });
}

// ---- join с проверками ----
inline void safe_join(std::thread& t) noexcept {
    if (t.joinable()) t.join(); // join() не бросает; на не-joinable вызов привёл бы к terminate()
}

struct SimpleStruct {
    int x;
    float y;

    std::vector<uint8_t> to_bytes() const {
        std::vector<uint8_t> bytes(sizeof(int) + sizeof(float));
        std::memcpy(bytes.data(), &x, sizeof(int));
        std::memcpy(bytes.data() + sizeof(int), &y, sizeof(float));
        return bytes;
    }

    static SimpleStruct from_bytes(const void* data, size_t size) {
        if (size != (sizeof(int) + sizeof(float))) throw std::runtime_error("Invalid data size for SimpleStruct");
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
    int value;

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

    bool operator==(const ConcurrentStruct& other) const {
        return value == other.value;
    }
    
    bool operator!=(const ConcurrentStruct& other) const {
        return value != other.value;
    }
};

int main() {
    mdbxc::Config cfg;
    cfg.pathname = "./data/testdb";
    cfg.max_dbs = 14;
    cfg.no_subdir = false;
    cfg.relative_to_exe = true;

    auto conn = mdbxc::Connection::create(cfg);

    // int8 -> int8
    std::cout << "int8 -> int8" << std::endl;
    {
        mdbxc::KeyValueTable<int8_t, int8_t> kv(conn, "i8_i8");
        kv.insert_or_assign(1, 100);
        ASSERT_FOUND(kv, 1, 100);
    }
    
    // int8 -> int64
    std::cout << "int8 -> int64" << std::endl;
    {
        mdbxc::KeyValueTable<int8_t, int64_t> kv(conn, "i8_i64");
        kv.insert_or_assign(2, 1234567890123456LL);
        ASSERT_FOUND(kv, 2, 1234567890123456LL);
    }

    // int32 -> string
    std::cout << "int32 -> string" << std::endl;
    {
        mdbxc::KeyValueTable<int32_t, std::string> kv(conn, "i32_str");
        kv.insert_or_assign(3, "hello");
        ASSERT_FOUND(kv, 3, std::string("hello"));
    }

    // string -> string
    std::cout << "string -> string" << std::endl;
    {
        mdbxc::KeyValueTable<std::string, std::string> kv(conn, "str_str");
        kv.insert_or_assign("key", "value");
        ASSERT_FOUND(kv, std::string("key"), std::string("value"));
    }

    // string -> POD
    std::cout << "string -> POD" << std::endl;
    {
        mdbxc::KeyValueTable<std::string, SimpleStruct> kv(conn, "str_struct");
        SimpleStruct s{42, 3.14f};
        kv.insert_or_assign("obj", s);
        ASSERT_FOUND(kv, std::string("obj"), s);
    }

    // int64 -> vector<byte>
    std::cout << "int64 -> vector<byte>" << std::endl;
    {
        mdbxc::KeyValueTable<int64_t, std::vector<uint8_t>> kv(conn, "i64_vec");
        std::vector<uint8_t> data{1, 2, 3, 4};
        kv.insert_or_assign(9, data);
        ASSERT_FOUND(kv, 9, data);
    }
    
    // string -> vector<SimpleStruct>
    std::cout << "string -> vector<SimpleStruct>" << std::endl;
    {
        mdbxc::KeyValueTable<std::string, std::vector<SimpleStruct>> kv(conn, "str_vec_struct");
        std::vector<SimpleStruct> vec{{1, 1.0f}, {2, 2.0f}};
        kv.insert_or_assign("many", vec);
        ASSERT_FOUND(kv, std::string("many"), vec);
    }
    
    // string -> list<string>
    std::cout << "string -> list<string>" << std::endl;
    {
        mdbxc::KeyValueTable<std::string, std::list<std::string>> kv(conn, "str_list_str");
        std::list<std::string> lst{"a", "b", "c"};
        kv.insert_or_assign("letters", lst);
        ASSERT_FOUND(kv, std::string("letters"), lst);
    }
    
    // string -> vector<string>
    std::cout << "string -> vector<string>" << std::endl;
    {
        mdbxc::KeyValueTable<std::string, std::vector<std::string>> kv(conn, "str_vector_str");
        std::vector<std::string> lst{"a", "b", "c"};
        kv.insert_or_assign("letters", lst);
        ASSERT_FOUND(kv, std::string("letters"), lst);
    }
    
    // string -> set<string>
    std::cout << "string -> set<string>" << std::endl;
    {
        mdbxc::KeyValueTable<std::string, std::set<std::string>> kv(conn, "str_set_str");
        std::set<std::string> s{"a", "b", "c"};
        kv.insert_or_assign("letters", s);
        ASSERT_FOUND(kv, std::string("letters"), s);
    }
    
    // string -> set<int>
    std::cout << "string -> set<int>" << std::endl;
    {
        mdbxc::KeyValueTable<std::string, std::set<int>> kv(conn, "str_set_int");
        std::set<int> s{1, 2, 3};
        kv.insert_or_assign("digits", s);
        ASSERT_FOUND(kv, std::string("digits"), s);
    }

    // string -> self-serializable struct
    std::cout << "string -> self-serializable struct" << std::endl;
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

    std::cout << "Concurrent test" << std::endl;
    {
        mdbxc::KeyValueTable<int, ConcurrentStruct> kv(conn, "concurrent_test");

        std::mutex mtx;
        std::condition_variable cv;
        std::size_t epoch = 0;
        std::size_t last_seen = 0;
        std::atomic<bool> failed(false);
        ConcurrentStruct written;

        std::exception_ptr w_ex, r_ex;

        auto writer = make_thread_catching([&kv, &epoch, &written, &cv, &mtx, &failed] {
            try {
                for (int i = 0; i < 1000; ++i) {
                    if (failed) break;
                    ConcurrentStruct w{i};
                    sync_cout() << "Thread 1 -> i=" << i << std::endl;
                    kv.insert_or_assign(1, w);
                    {
                        std::lock_guard<std::mutex> lock(mtx);
                        written = w;
                        ++epoch;
                    }
                    cv.notify_one();
                    std::this_thread::sleep_for(std::chrono::milliseconds(1));
                }
                sync_cout() << "Thread 1 end" << std::endl;
            } catch (const std::exception& ex) {
                sync_cerr() << "Writer thread exception: " << ex.what() << std::endl;
                failed = true;
            } catch (...) {
                sync_cerr() << "Writer thread unknown exception" << std::endl;
                failed = true;
            }
        }, w_ex);

        auto reader = make_thread_catching([&kv, &epoch, &last_seen, &failed, &written, &cv, &mtx] {
            try {
                for (int i = 0; i < 1000; ++i) {
                    std::unique_lock<std::mutex> lock(mtx);
                    bool ok = cv.wait_for(lock, std::chrono::seconds(2), [&epoch, &last_seen] {
                        return epoch > last_seen;
                    });
                    if (!ok) {
                        sync_cerr() << "Reader timeout at iteration " << i
                                  << " (epoch=" << epoch << ", last_seen=" << last_seen << ")\n";
                        failed = true;
                        break;
                    }
                    last_seen = epoch;
                    ConcurrentStruct expected = written; // снимок под тем же lock
                    lock.unlock();

#               if __cplusplus >= 201703L
                    //sync_cout() << "Thread 2 -> i=" << i << std::endl;
                    auto val = kv.find(1);
                    if (!val.has_value() || val.value() != expected) {
                        sync_cerr() << "Mismatch at iteration " << i
                                  << ": got " << (val ? val->value : -1)
                                  << ", expected " << expected.value << std::endl;
                        failed = true;
                        break;
                    }
#               else
                    auto val = kv.find_compat(1);
                    if (!val.first || val.second != expected) {
                        sync_cerr() << "Mismatch at iteration " << i
                                  << ": got " << val.second.value
                                  << ", expected " << expected.value << std::endl;
                        failed = true;
                        break;
                    }
#               endif
                }
                sync_cout() << "Thread 2 end" << std::endl;
            } catch (const std::exception& ex) {
                sync_cerr() << "Reader thread exception: " << ex.what() << std::endl;
                failed = true;
            } catch (...) {
                sync_cerr() << "Reader thread unknown exception" << std::endl;
                failed = true;
            }
        }, r_ex);

        safe_join(writer);
        safe_join(reader);
        
        if (w_ex) {
            try { std::rethrow_exception(w_ex); }
            catch (const std::exception& e) { sync_cerr() << "Writer exception: " << e.what() << "\n"; }
            catch (...) { sync_cerr() << "Writer unknown exception\n"; }
        }
        if (r_ex) {
            try { std::rethrow_exception(r_ex); }
            catch (const std::exception& e) { sync_cerr() << "Reader exception: " << e.what() << "\n"; }
            catch (...) { sync_cerr() << "Reader unknown exception\n"; }
        }

        if (w_ex || r_ex) {
            sync_cerr() << "Concurrent test failed (thread exception)\n";
            return 1;
        }

        if (failed) {
            std::cerr << "Concurrent test failed." << std::endl;
            return 1;
        }

        std::cout << "Concurrent test passed." << std::endl;
    }

    std::cout << "All tests passed." << std::endl;
    return 0;
}
