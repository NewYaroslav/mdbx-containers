#include <iostream>
#include <mdbx_containers/KeyValueContainer.hpp>

int main() {
    // Конфиг и подключение к БД
    mdbxc::Config config;
    config.pathname = "example_db.mdbx";

    mdbxc::KeyValueContainer<std::string, int> kv(config);
    
	std::cout << "-1" << std::endl;
    kv.clear();
	std::cout << "-2" << std::endl;
	
    // Присваивание значений (вставка или замена)
    kv["apple"] = 10;
    kv["banana"] = 25;
	
	std::cout << "-3" << std::endl;
	
    // Чтение значений
    int a = kv["apple"];     // 10
    int b = kv["banana"];    // 25
    int c = kv["unknown"];   // создаёт ключ "unknown" со значением 0 (как std::map)

	std::cout << "-4" << std::endl;

    // Изменение значения
    kv["apple"] = kv["apple"] + 1;
	
	std::cout << "-5" << std::endl;

    // Альтернатива: вставка, только если ключа нет
    kv.insert("pear", 100);
	
	std::cout << "-6" << std::endl;
	
    // Проверка наличия
    if (kv.contains("banana")) {
        std::cout << "banana = " << kv["banana"] << "\n";
    }
	
	std::cout << "-7" << std::endl;

    // Явное получение через optional
    auto val = kv.find("grape");
    if (val.has_value()) {
        std::cout << "grape = " << val.value() << "\n";
    } else {
        std::cout << "no grape in db\n";
    }
	
	std::cout << "-8" << std::endl;

    // Удаление
    kv.erase("banana");

    // Подсчёт всех ключей
    std::cout << "DB count = " << kv.count() << "\n";
}