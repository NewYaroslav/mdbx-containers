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
  - `readonly`, `writemap`, `readahead`, `max_readers`, `max_dbs`, `relative_to_exe`, `MDBX_NOSUBDIR`.

### ⚙️ Совместимость и подключение
- Заголовочная библиотека (header-only);
- Требуется только [libmdbx](https://github.com/erthink/libmdbx);
- Совместимость: C++11 и выше.

---

## 🔧 Установка

1. Добавьте исходники `mdbx-containers` в ваш проект.
2. Подключите `libmdbx` (через `find_package`, CMake subdirectory, или как сабмодуль).
3. Убедитесь, что компилятор поддерживает C++11 или выше.

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