#include <iostream>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <atomic>
#include <cassert>
#include <mdbx_containers/KeyValueTable.hpp>


struct SimpleStruct {
    int x;
    float y;

    std::vector<uint8_t> to_bytes() const {
        std::vector<uint8_t> bytes(sizeof(SimpleStruct));
        std::memcpy(bytes.data(), this, sizeof(SimpleStruct));
        return bytes;
    }

    static SimpleStruct from_bytes(const std::vector<uint8_t>& bytes) {
        SimpleStruct s{};
        std::memcpy(&s, bytes.data(), sizeof(SimpleStruct));
        return s;
    }
    
    static SimpleStruct from_bytes(const void* data, size_t size) {
        if (size != (sizeof(int) + sizeof(float)))
            throw std::runtime_error("Invalid data size for SimpleStruct");
        const uint8_t* ptr = static_cast<const uint8_t*>(data);
        SimpleStruct s{};
        std::memcpy(&s, ptr, sizeof(SimpleStruct));
        return s;
    }

    bool operator==(const SimpleStruct& other) const {
        return x == other.x && y == other.y;
    }
};

struct ConcurrentStruct {
    int value;

    std::vector<uint8_t> to_bytes() const {
        std::vector<uint8_t> bytes(sizeof(ConcurrentStruct));
        std::memcpy(bytes.data(), this, sizeof(ConcurrentStruct));
        return bytes;
    }

    static ConcurrentStruct from_bytes(const void* data, size_t size) {
        if (size != sizeof(ConcurrentStruct))
            throw std::runtime_error("Invalid data size");
        ConcurrentStruct s{};
        std::memcpy(&s, data, sizeof(ConcurrentStruct));
        return s;
    }

    bool operator==(const ConcurrentStruct& other) const {
        return value == other.value;
    }
    
    bool operator!=(const ConcurrentStruct& other) const {
        return !(*this == other);
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
    std::cout << "int8 -> int8\n";
    {
        mdbxc::KeyValueTable<int8_t, int8_t> kv(conn, "i8_i8");
        kv.insert_or_assign(1, 100);
        assert(kv.find(1).value() == 100);
    }
    
    // int8 -> int64
    std::cout << "int8 -> int64\n";
    {
        mdbxc::KeyValueTable<int8_t, int64_t> kv(conn, "i8_i64");
        kv.insert_or_assign(2, 1234567890123456LL);
        assert(kv.find(2).value() == 1234567890123456LL);
    }
    
    // int32 -> string
    std::cout << "int32 -> string\n";
    {
        mdbxc::KeyValueTable<int32_t, std::string> kv(conn, "i32_str");
        kv.insert_or_assign(3, "hello");
        assert(kv.find(3).value() == "hello");
    }

    // string -> string
    std::cout << "string -> string\n";
    {
        mdbxc::KeyValueTable<std::string, std::string> kv(conn, "str_str");
        kv.insert_or_assign("key", "value");
        assert(kv.find("key").value() == "value");
    }

    // string -> POD
    std::cout << "string -> POD\n";
    {
        mdbxc::KeyValueTable<std::string, SimpleStruct> kv(conn, "str_struct");
        SimpleStruct s{42, 3.14f};
        kv.insert_or_assign("obj", s);
        assert(kv.find("obj").value() == s);
    }

    // int64 -> vector<byte>
    std::cout << "int64 -> vector<byte>\n";
    {
        mdbxc::KeyValueTable<int64_t, std::vector<uint8_t>> kv(conn, "i64_vec");
        std::vector<uint8_t> data{1, 2, 3, 4};
        kv.insert_or_assign(9, data);
        assert(kv.find(9).value() == data);
    }
    
    // string -> vector<SimpleStruct>
    std::cout << "string -> vector<SimpleStruct>\n";
    {
        mdbxc::KeyValueTable<std::string, std::vector<SimpleStruct>> kv(conn, "str_vec_struct");
        std::vector<SimpleStruct> vec{{1, 1.0f}, {2, 2.0f}};
        kv.insert_or_assign("many", vec);
        assert(kv.find("many").value() == vec);
    }
    
    // string -> list<string>
    std::cout << "string -> list<string>\n";
    {
        mdbxc::KeyValueTable<std::string, std::list<std::string>> kv(conn, "str_list_str");
        std::list<std::string> lst{"a", "b", "c"};
        kv.insert_or_assign("letters", lst);
        assert(kv.find("letters").value() == lst);
    }
    
    // string -> vector<string>
    std::cout << "string -> vector<string>\n";
    {
        mdbxc::KeyValueTable<std::string, std::vector<std::string>> kv(conn, "str_vector_str");
        std::vector<std::string> lst{"a", "b", "c"};
        kv.insert_or_assign("letters", lst);
        assert(kv.find("letters").value() == lst);
    }
    
    // string -> set<string>
    std::cout << "string -> set<string>\n";
    {
        mdbxc::KeyValueTable<std::string, std::set<std::string>> kv(conn, "str_set_str");
        std::set<std::string> s{"a", "b", "c"};
        kv.insert_or_assign("letters", s);
        assert(kv.find("letters").value() == s);
    }
    
    // string -> set<int>
    std::cout << "string -> set<int>\n";
    {
        mdbxc::KeyValueTable<std::string, std::set<int>> kv(conn, "str_set_int");
        std::set<int> s{1, 2, 3};
        kv.insert_or_assign("digits", s);
        assert(kv.find("digits").value() == s);
    }

    // string -> self-serializable struct
    std::cout << "string -> self-serializable struct\n";
    {
        struct Serializable {
            int a = 0;
            std::string b;

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
        assert(kv.find("ser").value() == s);
    }
    
    {
        mdbxc::KeyValueTable<int, ConcurrentStruct> kv(conn, "concurrent_test");

        std::mutex mtx;
        std::condition_variable cv;
        std::atomic<bool> ready = false;
        std::atomic<bool> failed = false;
        ConcurrentStruct written;

        std::thread writer([&kv, &ready, &written, &cv, &mtx] {
            for (int i = 0; i < 100; ++i) {
                {
                    std::lock_guard<std::mutex> lock(mtx);
                    written = ConcurrentStruct{i};
                    kv.insert_or_assign(1, written);
                    ready = true;
                }
                cv.notify_one();
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
        });

        std::thread reader([&kv, &ready, &failed, &written, &cv, &mtx] {
            for (int i = 0; i < 100; ++i) {
                std::unique_lock<std::mutex> lock(mtx);
                cv.wait(lock, [&] { return ready.load(); });

                auto val = kv.find(1);
                if (!val.has_value() || val.value() != written) {
                    std::cerr << "Mismatch at iteration " << i << ": got " << val.value().value << ", expected " << written.value << std::endl;
                    failed = true;
                    break;
                }

                ready = false;
            }
        });

        writer.join();
        reader.join();

        if (failed) {
            std::cerr << "Concurrent test failed." << std::endl;
            return 1;
        }

        std::cout << "Concurrent test passed." << std::endl;
        
    }

    std::cout << "All tests passed.\n";
    std::cin.ignore(std::numeric_limits<std::streamsize>::max(), '\n');
    std::cin.get();
    return 0;
}