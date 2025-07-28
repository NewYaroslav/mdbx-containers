#include <iostream>
#include "mdbx.h"
#include <vector>

uint64_t generate_tick_key(uint32_t asset_id, uint16_t provider_id, uint16_t unix_hour) {
    return ((uint64_t)asset_id << 32) | ((uint64_t)provider_id << 16) | (uint64_t)unix_hour;
}

int main() {
    int rc;
    MDBX_env *env = nullptr;
    MDBX_dbi dbi;
    MDBX_dbi dbi_ticks, dbi_metadata;
    MDBX_val key, data;
    MDBX_txn *txn = nullptr;
    MDBX_cursor *cursor = nullptr;
    char skey[32];
    char sval[32];

    std::vector<uint8_t> tick_data(16, 'D'); // Бинарные данные тиков
    uint64_t tick_key = 0;


    printf("MDBX limits:\n");
#if UINTPTR_MAX > 0xffffFFFFul || ULONG_MAX > 0xffffFFFFul
    const double scale_factor = 1099511627776.0;
    const char *const scale_unit = "TiB";
#else
    const double scale_factor = 1073741824.0;
    const char *const scale_unit = "GiB";
#endif
    const size_t pagesize_min = mdbx_limits_pgsize_min();
    const size_t pagesize_max = mdbx_limits_pgsize_max();
    const size_t pagesize_default = mdbx_default_pagesize();

    printf("\tPage size: a power of 2, minimum %zu, maximum %zu bytes,"
         " default %zu bytes.\n",
         pagesize_min, pagesize_max, pagesize_default);
    printf("\tKey size: minimum %zu, maximum ≈¼ pagesize (%zu bytes for default"
         " %zuK pagesize, %zu bytes for %zuK pagesize).\n",
         (size_t)0, mdbx_limits_keysize_max(-1, MDBX_DB_DEFAULTS), pagesize_default / 1024,
         mdbx_limits_keysize_max(pagesize_max, MDBX_DB_DEFAULTS), pagesize_max / 1024);
    printf("\tValue size: minimum %zu, maximum %zu (0x%08zX) bytes for maps,"
         " ≈¼ pagesize for multimaps (%zu bytes for default %zuK pagesize,"
         " %zu bytes for %zuK pagesize).\n",
         (size_t)0, mdbx_limits_valsize_max(pagesize_min, MDBX_DB_DEFAULTS),
         mdbx_limits_valsize_max(pagesize_min, MDBX_DB_DEFAULTS), mdbx_limits_valsize_max(-1, MDBX_DUPSORT),
         pagesize_default / 1024, mdbx_limits_valsize_max(pagesize_max, MDBX_DUPSORT), pagesize_max / 1024);
    printf("\tWrite transaction size: up to %zu (0x%zX) pages (%f %s for default "
         "%zuK pagesize, %f %s for %zuK pagesize).\n",
         mdbx_limits_txnsize_max(pagesize_min) / pagesize_min, mdbx_limits_txnsize_max(pagesize_min) / pagesize_min,
         mdbx_limits_txnsize_max(-1) / scale_factor, scale_unit, pagesize_default / 1024,
         mdbx_limits_txnsize_max(pagesize_max) / scale_factor, scale_unit, pagesize_max / 1024);
    printf("\tDatabase size: up to %zu pages (%f %s for default %zuK "
         "pagesize, %f %s for %zuK pagesize).\n",
         mdbx_limits_dbsize_max(pagesize_min) / pagesize_min, mdbx_limits_dbsize_max(-1) / scale_factor, scale_unit,
         pagesize_default / 1024, mdbx_limits_dbsize_max(pagesize_max) / scale_factor, scale_unit, pagesize_max / 1024);
    printf("\tMaximum sub-databases: %u.\n", MDBX_MAX_DBI);
    printf("-----\n");

    // Создаем окружение
    rc = mdbx_env_create(&env);
    if (rc != MDBX_SUCCESS) {
        fprintf(stderr, "mdbx_env_create: (%d) %s\n", rc, mdbx_strerror(rc));
        goto bailout;
    }

    // Настраиваем геометрию базы данных
    rc = mdbx_env_set_geometry(env,
                               -1, // size_lower
                               -1, // Размер в байтах для настройки размера базы данных на данный момент. Нулевое значение означает "минимально приемлемый", а отрицательное означает "сохранить текущий или использовать значение по умолчанию". Поэтому рекомендуется всегда передавать -1 в этом аргументе, за исключением некоторых особых случаев.
                               -1, // size_upper
                               16 * 1024 * 1024, // growth_step
                               16 * 1024 * 1024, // shrink_threshold
                               0); // pagesize (64 * 1024)
    if (rc != MDBX_SUCCESS) {
        fprintf(stderr, "mdbx_env_set_geometry: (%d) %s\n", rc, mdbx_strerror(rc));
        goto bailout;
    }

    // Устанавливаем лимит на 10 карт
    rc = mdbx_env_set_maxdbs(env, 10);
    if (rc != MDBX_SUCCESS) {
        fprintf(stderr, "mdbx_env_set_maxdbs: (%d) %s\n", rc, mdbx_strerror(rc));
        goto bailout;
    }

    // Открываем окружение
    rc = mdbx_env_open(env, "./example-db", MDBX_NOSUBDIR, 0664);
    if (rc != MDBX_SUCCESS) {
        fprintf(stderr, "mdbx_env_open: (%d) %s\n", rc, mdbx_strerror(rc));
        goto bailout;
    }

    // Начинаем транзакцию
    rc = mdbx_txn_begin(env, nullptr, MDBX_TXN_READWRITE, &txn);
    if (rc != MDBX_SUCCESS) {
        fprintf(stderr, "mdbx_txn_begin: (%d) %s\n", rc, mdbx_strerror(rc));
        goto bailout;
    }

    // Открываем (или создаем) базу данных
    rc = mdbx_dbi_open(txn, "temp", MDBX_DB_DEFAULTS | MDBX_CREATE, &dbi);
    if (rc != MDBX_SUCCESS) {
        fprintf(stderr, "mdbx_dbi_open: (%d) %s\n", rc, mdbx_strerror(rc));
        goto bailout;
    }

    // Вставляем данные
    sprintf(skey, "key");
    sprintf(sval, "value");
    key.iov_len = sizeof("key");
    key.iov_base = skey;
    data.iov_len = sizeof(sval);
    data.iov_base = sval;
    rc = mdbx_put(txn, dbi, &key, &data, MDBX_UPSERT);
    if (rc != MDBX_SUCCESS) {
        fprintf(stderr, "mdbx_put: (%d) %s\n", rc, mdbx_strerror(rc));
        goto bailout;
    }

    // Завершаем транзакцию
    rc = mdbx_txn_commit(txn);
    if (rc != MDBX_SUCCESS) {
        fprintf(stderr, "mdbx_txn_commit: (%d) %s\n", rc, mdbx_strerror(rc));
        goto bailout;
    }
    txn = nullptr;

    // Начинаем транзакцию для чтения данных
    rc = mdbx_txn_begin(env, nullptr, MDBX_TXN_RDONLY, &txn);
    if (rc != MDBX_SUCCESS) {
        fprintf(stderr, "mdbx_txn_begin: (%d) %s\n", rc, mdbx_strerror(rc));
        goto bailout;
    }

    // Открываем курсор для чтения данных
    rc = mdbx_cursor_open(txn, dbi, &cursor);
    if (rc != MDBX_SUCCESS) {
        fprintf(stderr, "mdbx_cursor_open: (%d) %s\n", rc, mdbx_strerror(rc));
        goto bailout;
    }

    // Читаем и выводим данные
    while ((rc = mdbx_cursor_get(cursor, &key, &data, MDBX_NEXT)) == MDBX_SUCCESS) {
        printf("key: %.*s, data: %.*s\n",
               (int)key.iov_len, (char *)key.iov_base,
               (int)data.iov_len, (char *)data.iov_base);
    }
    if (rc != MDBX_NOTFOUND) {
        fprintf(stderr, "mdbx_cursor_get: (%d) %s\n", rc, mdbx_strerror(rc));
        goto bailout;
    }
    if (cursor) mdbx_cursor_close(cursor);

    // Завершаем транзакцию
    rc = mdbx_txn_abort(txn);
    if (rc != MDBX_SUCCESS) {
        fprintf(stderr, "mdbx_txn_abort: (%d) %s\n", rc, mdbx_strerror(rc));
        goto bailout;
    }
    txn = nullptr;


//------------------------------------------------------------------------------

    // Начинаем транзакцию
    rc = mdbx_txn_begin(env, nullptr, MDBX_TXN_READWRITE, &txn);
    if (rc != MDBX_SUCCESS) {
        fprintf(stderr, "mdbx_txn_begin: (%d) %s\n", rc, mdbx_strerror(rc));
        goto bailout;
    }

    // Открываем карту для тиков
    rc = mdbx_dbi_open(txn, "ticks", MDBX_DB_DEFAULTS | MDBX_INTEGERKEY | MDBX_CREATE, &dbi_ticks);
    if (rc != MDBX_SUCCESS) {
        fprintf(stderr, "mdbx_dbi_open (ticks): (%d) %s\n", rc, mdbx_strerror(rc));
        goto bailout;
    }

    tick_key = generate_tick_key(1, 1, 1);
    MDBX_val mdbx_key, mdbx_data;
    mdbx_key.iov_base = &tick_key;
    mdbx_key.iov_len = sizeof(tick_key);

    mdbx_data.iov_base = tick_data.data();
    mdbx_data.iov_len = tick_data.size();

    rc = mdbx_put(txn, dbi_ticks, &mdbx_key, &mdbx_data, MDBX_UPSERT);
    if (rc != MDBX_SUCCESS) {
        fprintf(stderr, "mdbx_put: (%d) %s\n", rc, mdbx_strerror(rc));
        goto bailout;
    }

    tick_key = generate_tick_key(2, 1, 1);
    mdbx_key.iov_base = &tick_key;
    mdbx_key.iov_len = sizeof(tick_key);
    tick_data[0] = 'F';

    rc = mdbx_put(txn, dbi_ticks, &mdbx_key, &mdbx_data, MDBX_UPSERT);
    if (rc != MDBX_SUCCESS) {
        fprintf(stderr, "mdbx_put: (%d) %s\n", rc, mdbx_strerror(rc));
        goto bailout;
    }

    // Завершаем транзакцию
    rc = mdbx_txn_commit(txn);
    if (rc != MDBX_SUCCESS) {
        fprintf(stderr, "mdbx_txn_commit: (%d) %s\n", rc, mdbx_strerror(rc));
        goto bailout;
    }
    txn = nullptr;

    // Начинаем транзакцию для чтения данных
    rc = mdbx_txn_begin(env, nullptr, MDBX_TXN_RDONLY, &txn);
    if (rc != MDBX_SUCCESS) {
        fprintf(stderr, "mdbx_txn_begin: (%d) %s\n", rc, mdbx_strerror(rc));
        goto bailout;
    }

    // Открываем курсор для чтения данных
    rc = mdbx_cursor_open(txn, dbi_ticks, &cursor);
    if (rc != MDBX_SUCCESS) {
        fprintf(stderr, "mdbx_cursor_open: (%d) %s\n", rc, mdbx_strerror(rc));
        goto bailout;
    }

    // Читаем и выводим данные
    while ((rc = mdbx_cursor_get(cursor, &key, &data, MDBX_NEXT)) == MDBX_SUCCESS) {
        printf("key: %.*s, data: %.*s\n",
               (int)key.iov_len, (char *)key.iov_base,
               (int)data.iov_len, (char *)data.iov_base);
    }
    if (rc != MDBX_NOTFOUND) {
        fprintf(stderr, "mdbx_cursor_get: (%d) %s\n", rc, mdbx_strerror(rc));
        goto bailout;
    }

    // Завершаем транзакцию
    rc = mdbx_txn_abort(txn);
    if (rc != MDBX_SUCCESS) {
        fprintf(stderr, "mdbx_txn_abort: (%d) %s\n", rc, mdbx_strerror(rc));
        goto bailout;
    }
    txn = nullptr;


//------------------------------------------------------------------------------

bailout:
    // Освобождаем ресурсы
    if (cursor)
        mdbx_cursor_close(cursor);
    if (txn)
        mdbx_txn_abort(txn);
    if (env)
        mdbx_env_close(env);

    return (rc != MDBX_SUCCESS) ? EXIT_FAILURE : EXIT_SUCCESS;
}
