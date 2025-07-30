# mdbx-containers

**mdbx-containers** — это лёгкая заголовочная библиотека на C++11/C++17, предоставляющая удобный интерфейс для работы с базой данных [libmdbx](https://github.com/erthink/libmdbx) через стандартные контейнеры STL: `std::map`, `std::set`, `std::unordered_map`, `std::vector` и др.

Библиотека позволяет прозрачно синхронизировать in-memory контейнеры с данными в MDBX, обеспечивая высокую производительность, надежную транзакционность и поддержку конкурентного доступа.

---

## ✨ Особенности

### 🧩 Унифицированный API
- Одинаковый интерфейс для всех таблиц: `insert`, `insert_or_assign`, `find`, `erase`, `clear`, `load`, `reconcile`, `operator[]` и др.
- Три типа таблиц:
  - `KeyTable<K>` — только ключи;
  - `KeyValueTable<K, V>` — один `V` на ключ;
  - `KeyMultiValueTable<K, V>` — несколько `V` на ключ (`std::multimap`).

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
2. Убедитесь, что `libmdbx` доступна системе (можно собрать автоматически при `BUILD_DEPS=ON`).
3. Проверьте поддержку стандарта C++11 и выше.

### Сборка через CMake

```bash
cmake -S . -B build \
    -DBUILD_DEPS=ON \
    -DBUILD_STATIC_LIB=ON \
    -DBUILD_TESTS=ON \
    -DBUILD_EXAMPLES=ON
cmake --build build
```


Для Windows доступны `.bat`‑скрипты с аналогичными параметрами (`build-17-examples.bat`, `build-mingw-17-tests.bat` и др.).

---

## 🚀 Пример использования

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

    // Запись
    table.insert_or_assign(1, "Hello");
    table.insert_or_assign(2, "World");

    // Чтение
    std::map<int, std::string> result;
    table.load(result);

    for (const auto& pair : result)
        std::cout << pair.first << ": " << pair.second << "\n";

    return 0;
}
```

📘 Документация
- Подробные примеры см. в папке examples/.
- API и архитектура описаны в Wiki (если есть).
- Автоматическая генерация документации возможна с Doxygen.

🪪 Лицензия
Проект распространяется под лицензией MIT.
