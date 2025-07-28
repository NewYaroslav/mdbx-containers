# mdbx-containers

**mdbx-containers** — это лёгкая заголовочная библиотека на C++17, предоставляющая удобный интерфейс для работы с базой данных [libmdbx](https://github.com/erthink/libmdbx) через стандартные контейнеры STL: `std::map`, `std::set`, `std::unordered_map`, `std::vector` и др.

Библиотека позволяет прозрачно синхронизировать in-memory контейнеры с данными в MDBX, обеспечивая высокую производительность, надежную транзакционность и поддержку конкурентного доступа.

---

## ✨ Особенности

- **Интеграция с контейнерами:** Простая работа с `std::set`, `std::map`, `std::multimap`, `std::vector` и другими.
- **Поддержка ключей, пар и мультизаписей:** Аналоги `KeyDB`, `KeyValueDB`, `KeyMultiValueDB`.
- **Один файл БД — несколько таблиц (баз данных):** Каждый экземпляр может работать с отдельной под-БД (named DBI).
- **Транзакции и безопасность:** Поддержка вложенных транзакций и автоматическая сериализация операций.
- **Многопоточность:** Потокобезопасность благодаря архитектуре libmdbx.
- **Простота подключения:** Заголовочная библиотека без зависимостей, кроме MDBX.
- **Совместимость:** Стандарты C++17 и выше.

---

## 🔧 Установка

- Подключите исходники `mdbx-containers` в ваш проект.
- Убедитесь, что libmdbx доступен в вашей системе или подключён как сабмодуль.

---

## 🚀 Пример использования

```cpp
#include <mdbx_containers/KeyValueDB.hpp>
#include <iostream>
#include <map>

int main() {
    mdbx_containers::Config config;
    config.db_path = "example.mdbx";
    config.table_name = "map_data";
    mdbx_containers::KeyValueDB<int, std::string> kv_db(config);
    kv_db.connect();

    kv_db.insert(1, "Hello");
    kv_db.insert(2, "World");

    auto result = kv_db.retrieve_all<std::map>();
    for (auto& [k, v] : result)
        std::cout << k << ": " << v << "\n";
}
```

📘 Документация
Подробная документация и примеры доступны в папке examples и в Wiki проекта.

🪪 Лицензия
Проект распространяется под лицензией MIT.