# MDBX-Containers

**mdbx-containers** — это лёгкая заголовочная библиотека на C++11/C++17, предоставляющая удобный STL‑подобный интерфейс для работы с базой данных [libmdbx](https://github.com/erthink/libmdbx).

Библиотека позволяет прозрачно синхронизировать in-memory контейнеры с данными в MDBX, обеспечивая высокую производительность, надежную транзакционность и поддержку конкурентного доступа.


> Примечание  
> Этот проект оценивает техническое качество выше личных взглядов его авторов.  
> Подробности см. в [PHILOSOPHY.md](PHILOSOPHY.md).

---

## ⚙️ Особенности

### 🧱 API таблиц
- `KeyValueTable<K, V>` — основная реализованная таблица: один `V` на ключ, методы `insert`, `insert_or_assign`, `find`, `erase`, `clear`, `load`, `reconcile`, `operator[]` и др.
- `AnyValueTable<K>` — реализованная таблица для значений разных типов с типизированными методами `set`, `insert`, `get`, `find`, `get_or`, `update`, `contains`, `erase`, `keys`.
- `KeyTable<K>` — реализованная таблица только для уникальных ключей со `std::set`-подобным API.
- `KeyMultiValueTable<K, V>` — реализованная таблица для нескольких `V` на ключ (`std::multimap`), включая повторяющиеся одинаковые пары `(key, value)`.
- Проверка type-tag prefix в `AnyValueTable` пока реализована не полностью, поэтому не полагайтесь на неё как на полноценную runtime type safety.

### 🔁 Сериализация и типы
- Автоматическая сериализация:
  - trivially copyable типы — по памяти;
  - пользовательские — через `to_bytes()` / `from_bytes()`;
- Поддержка STL-контейнеров: `std::string`, `std::vector`, `std::list`, `std::set`, `std::vector<std::pair<K, V>>` и др.

### 🔒 Транзакции и многопоточность
- RAII-обёртка транзакций (`Transaction`);
- Привязка транзакции к потоку (`std::thread`);
- Потокобезопасность при работе с таблицами (через `TransactionTracker`, `std::mutex`).

### 🗄️ Структура и конфигурация
- Поддержка множества таблиц в одном MDBX-файле (через `MDBX_dbi`);
- Гибкая конфигурация:
  - `read_only`, `writemap_mode`, `readahead`, `no_subdir`, `sync_durable`,
    `max_readers`, `max_dbs`, `relative_to_exe`.
  - Подробности см. в файле `docs/configuration.dox`.

### 🧰 Совместимость и подключение
- Заголовочная библиотека (header-only);
- Требуется только [libmdbx](https://github.com/erthink/libmdbx);
- Совместимость: C++11 и выше.
- **Windows (MSVC)** пока не поддерживается. Используйте MinGW-w64 (GCC) или Clang под Windows.

---

## 🛠️ Установка и сборка

1. Скопируйте папку `include/` в проект или подключите репозиторий как submodule.
2. Убедитесь, что `libmdbx` доступна системе. Установите `MDBXC_DEPS_MODE=BUNDLED`, чтобы использовать bundled submodule в `external/libmdbx`, или `SYSTEM`/`AUTO` для установленного пакета.
3. Проверьте поддержку стандарта C++11 и выше.

### Сборка через CMake

```bash
cmake -S . -B build \
    -DMDBXC_DEPS_MODE=BUNDLED \
    -DMDBXC_BUILD_TESTS=ON \
    -DMDBXC_BUILD_EXAMPLES=ON \
    -DMDBXC_USE_ASAN=ON \
    -DCMAKE_CXX_STANDARD=17
cmake --build build
ctest --test-dir build --output-on-failure
```

> **Предупреждение**
> Собирайте все translation unit'ы, использующие mdbx-containers, с одинаковым
> стандартом C++, настройками выравнивания/упаковки структур и набором feature
> macros. Смешивание C++11 и C++17 или изменение ABI-чувствительных `#define`
> между файлами может привести к ODR-конфликтам и неопределённому поведению.

Для Windows доступны `.bat`‑скрипты с аналогичными параметрами (`build-mingw-17-examples.bat`, `build-mingw-17-tests.bat`, `build-mingw-11-tests.bat`).

---

## 🧪 Примеры использования

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

## 📚 Документация
- Подробные примеры см. в папке examples/.
- API и архитектура описаны в Doxygen-источниках `docs/*.dox`.
- Автоматическая генерация документации возможна с Doxygen; сгенерированные `docs/html/` и `docs/latex/` не редактируются вручную.

## 📄 Лицензия
Проект распространяется под лицензией MIT.

Проект может использовать bundled [libmdbx](https://github.com/erthink/libmdbx) из `external/libmdbx`; библиотека распространяется по лицензии Apache License 2.0. Файл лицензии расположен в `docs/libmdbx.LICENSE`.
