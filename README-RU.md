# mdbx-containers

**mdbx-containers** — это лёгкая заголовочная библиотека на C++11/C++17, предоставляющая удобный интерфейс для работы с базой данных [libmdbx](https://github.com/erthink/libmdbx) через стандартные контейнеры STL: `std::map`, `std::set`, `std::unordered_map`, `std::vector` и др.

Библиотека позволяет прозрачно синхронизировать in-memory контейнеры с данными в MDBX, обеспечивая высокую производительность, надежную транзакционность и поддержку конкурентного доступа.

---

## ✨ Особенности

### 🧩 Унифицированный API
- Одинаковый интерфейс для всех таблиц: `insert`, `insert_or_assign`, `find`, `erase`, `clear`, `load`, `reconcile`, `operator[]` и др.
- Четыре типа таблиц:
  - `KeyTable<K>` — только ключи;
  - `KeyValueTable<K, V>` — один `V` на ключ;
  - `KeyMultiValueTable<K, V>` — несколько `V` на ключ (`std::multimap`);
  - `AnyValueTable<K>` — хранение значений произвольного типа.

### 🔄 Сериализация и типы
- Автоматическая сериализация:
  - trivially copyable типы — по памяти;
  - пользовательские — через `to_bytes()` / `from_bytes()`;
- Поддержка STL-контейнеров: `std::string`, `std::vector`, `std::list`, `std::set`, `std::vector<std::pair<K, V>>` и др.

### 🧵 Транзакции и многопоточность
- RAII-обёртка транзакций (`Transaction`);
- Привязка транзакции к потоку (`std::thread`);
- Потокобезопасность при работе с таблицами (через `TransactionTracker`, `std::mutex`).

### 🗃️ Структура и конфигурация
- Поддержка множества таблиц в одном MDBX-файле (через `MDBX_dbi`);
- Гибкая конфигурация:
  - `read_only`, `writemap_mode`, `readahead`, `no_subdir`, `sync_durable`,
    `max_readers`, `max_dbs`, `relative_to_exe`.
  - Подробности см. в файле `docs/configuration.dox`.

### ⚙️ Совместимость и подключение
- Заголовочная библиотека (header-only);
- Требуется только [libmdbx](https://github.com/erthink/libmdbx);
- Совместимость: C++11 и выше.

---

## 🔧 Установка и сборка

1. Скопируйте папку `include/` в проект или подключите репозиторий как submodule.
2. Убедитесь, что `libmdbx` доступна системе (установите `MDBXC_DEPS_MODE=BUNDLED` для автоматической сборки).
3. Проверьте поддержку стандарта C++11 и выше.

### Сборка через CMake

```bash
cmake -S . -B build \
    -DMDBXC_DEPS_MODE=BUNDLED \
    -DMDBXC_BUILD_STATIC_LIB=ON \
    -DMDBXC_BUILD_TESTS=ON \
    -DMDBXC_BUILD_EXAMPLES=ON \
    -DMDBXC_USE_ASAN=ON \
    -DCMAKE_CXX_STANDARD=17
cmake --build build
ctest --test-dir build --output-on-failure
```


Для Windows доступны `.bat`‑скрипты с аналогичными параметрами (`build-mingw-17-examples.bat`, `build-mingw-17-tests.bat`, `build-mingw-11-tests.bat`).

---

## 🚀 Примеры использования

### Базовая таблица ключ-значение

```cpp
#include <mdbx_containers/KeyValueTable.hpp>
#include <iostream>
#include <map>

int main() {
    mdbxc::Config config;
    config.pathname = "example.mdbx";
    config.max_dbs = 4;

    auto conn = mdbxc::Connection::create(config);
    mdbxc::KeyValueTable<int, std::string> table(conn, "my_map");

    table.insert_or_assign(1, "Hello");
    table.insert_or_assign(2, "World");

    std::map<int, std::string> result;
    table.load(result);

    for (const auto& pair : result)
        std::cout << pair.first << ": " << pair.second << "\n";

    return 0;
}
```

### Ручное управление транзакцией

```cpp
mdbxc::Config config;
config.pathname = "txn.mdbx";
auto conn = mdbxc::Connection::create(config);
mdbxc::KeyValueTable<int, std::string> table(conn, "demo");

conn->begin(mdbxc::TransactionMode::WRITABLE);
table.insert_or_assign(10, "ten");
conn->commit();
```

### Пользовательская структура

```cpp
struct MyData {
    int id;
    double value;

    std::vector<uint8_t> to_bytes() const {
        std::vector<uint8_t> bytes(sizeof(MyData));
        std::memcpy(bytes.data(), this, sizeof(MyData));
        return bytes;
    }

    static MyData from_bytes(const void* data, size_t size) {
        MyData out{};
        std::memcpy(&out, data, sizeof(MyData));
        return out;
    }
};

mdbxc::KeyValueTable<int, MyData> table(conn, "my_data");
table.insert_or_assign(42, MyData{42, 3.14});
```

📘 Документация
- Подробные примеры см. в папке examples/.
- API и архитектура описаны в Wiki (если есть).
- Автоматическая генерация документации возможна с Doxygen.

🪪 Лицензия
Проект распространяется под лицензией MIT.

В состав репозитория включена библиотека [libmdbx](https://github.com/erthink/libmdbx), распространяемая по лицензии Apache License 2.0. Файл лицензии расположен в `docs/libmdbx.LICENSE`.
