# MDBX-Containers

[![MIT License](https://img.shields.io/badge/license-MIT-green.svg)](LICENSE) ![Platform](https://img.shields.io/badge/platform-Windows%20%7C%20Linux%20%7C%20macOS-blue) ![C++ Standard](https://img.shields.io/badge/C++-11--17-orange) [![CI Windows](https://img.shields.io/github/actions/workflow/status/NewYaroslav/mdbx-containers/ci.yml?branch=main&label=Windows&logo=windows)](https://github.com/NewYaroslav/mdbx-containers/actions/workflows/ci.yml) [![CI Linux](https://img.shields.io/github/actions/workflow/status/NewYaroslav/mdbx-containers/ci.yml?branch=main&label=Linux&logo=linux)](https://github.com/NewYaroslav/mdbx-containers/actions/workflows/ci.yml) [![CI macOS](https://img.shields.io/github/actions/workflow/status/NewYaroslav/mdbx-containers/ci.yml?branch=main&label=macOS&logo=apple)](https://github.com/NewYaroslav/mdbx-containers/actions/workflows/ci.yml)

[English version](README.md)

**mdbx-containers** — лёгкая заголовочная библиотека C++11/17, которая соединяет
[libmdbx](https://github.com/erthink/libmdbx) с привычными STL-подобными API.
Она сохраняет key-value данные в MDBX и при этом предоставляет высокую
производительность и удобные помощники для транзакций.

> Примечание  
> В этом проекте техническое качество важнее личных взглядов авторов.
> Подробнее см. [PHILOSOPHY.md](PHILOSOPHY.md).

## ⚙️ Особенности

### 🧱 API таблиц
- `KeyValueTable<K, V>` — основная реализованная таблица: одно значение на ключ,
  методы `insert`, `insert_or_assign`, `find`, `erase`, `clear`, `load`,
  `reconcile`, `operator[]` и связанные помощники.
- `AnyValueTable<K>` хранит значения разных типов по выбранному вызывающим кодом
  типу и поддерживает типизированные `set`, `insert`, `get`, `find`, `get_or`,
  `update`, `contains`, `erase` и `keys`.
- `KeyTable<K>` хранит уникальные ключи со `std::set`-подобным API: `insert`,
  `contains`, `erase`, `clear`, `load`, `reconcile` и связанные помощники.
- `KeyMultiValueTable<K, V>` хранит несколько значений на один ключ со
  `std::multimap`-подобным API и сохраняет повторяющиеся одинаковые пары
  `(key, value)`.
- Проверка type-tag prefix в `AnyValueTable` пока реализована не полностью, не
  полагайтесь на неё как на полноценную runtime type safety.

### 🔁 Сериализация
- Автоматическая сериализация trivially copyable типов.
- Пользовательские типы через `to_bytes()` / `from_bytes()`.
- Поддержка вложенных STL-контейнеров, например `std::vector` или `std::list`.

### 🔒 Транзакции и потоки
- RAII-транзакции (`Transaction`).
- Привязка транзакции к потоку.
- Безопасный конкурентный доступ через `TransactionTracker` и mutex'ы.

### 🗄️ Структура и конфигурация
- Несколько логических таблиц внутри одного MDBX-файла.
- Гибкая конфигурация: `read_only`, `writemap_mode`, `readahead`, `no_subdir`,
  `sync_durable`, `max_readers`, `max_dbs`, `relative_to_exe`.
- Подробнее см. `docs/configuration.dox`.

### 🧰 Совместимость
- Header-only использование.
- Зависит только от [libmdbx](https://github.com/erthink/libmdbx).
- Требует C++11 или новее.
- **Windows (MSVC)** пока не поддерживается. Используйте MinGW-w64 (GCC) или
  Clang под Windows.

## 🛠️ Установка

1. Скопируйте каталог `include/` в свой проект или подключите репозиторий как
   submodule.
2. Убедитесь, что `libmdbx` доступна вашей системе сборки. Установите
   `MDBXC_DEPS_MODE=BUNDLED`, чтобы использовать bundled submodule в
   `external/libmdbx`, или `SYSTEM`/`AUTO` для установленного пакета.
3. Используйте компилятор с поддержкой C++11 или новее.

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
> macros. Смешивание C++11 и C++17 сборок или изменение ABI-чувствительных
> define'ов между файлами может привести к ODR-нарушениям и неопределённому
> поведению.

Пользователи Windows могут запускать `.bat`-скрипты, например
`build-mingw-17-examples.bat`, `build-mingw-17-tests.bat` или
`build-mingw-11-tests.bat`.

## 🧪 Примеры использования

### Базовая key-value таблица

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

### Таблица только для ключей

```cpp
#include <mdbx_containers/KeyTable.hpp>
#include <set>

mdbxc::KeyTable<std::string> keys(conn, "tags");
keys.insert("active");
keys.insert("archived");

std::set<std::string> restored = keys.retrieve_all();
```

### Таблица с несколькими значениями на ключ

```cpp
#include <mdbx_containers/KeyMultiValueTable.hpp>

mdbxc::KeyMultiValueTable<int, std::string> events(conn, "events");
events.insert(7, "created");
events.insert(7, "created"); // одинаковые повторы сохраняются
events.insert(7, "sent");

std::vector<std::string> values = events.find(7);
```

### Ручная транзакция

```cpp
mdbxc::Config config;
config.pathname = "txn.mdbx";
auto conn = mdbxc::Connection::create(config);
mdbxc::KeyValueTable<int, std::string> table(conn, "demo");

conn->begin(mdbxc::TransactionMode::WRITABLE);
table.insert_or_assign(10, "ten");
conn->commit();
```

### Сериализация пользовательской структуры

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

- Больше примеров см. в каталоге `examples/`.
- Информация об API и архитектуре находится в Doxygen-страницах
  `docs/*.dox`.
- Документацию можно сгенерировать через Doxygen; сгенерированные
  `docs/html/` и `docs/latex/` нельзя редактировать вручную.

## 📄 Лицензия

Проект распространяется под лицензией MIT.

Проект может использовать bundled [libmdbx](https://github.com/erthink/libmdbx)
из `external/libmdbx`, распространяемую под Apache License 2.0. Подробнее см.
`docs/libmdbx.LICENSE`.
