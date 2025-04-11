#include "MemoryPool.h"
#include <gtest/gtest.h>

TEST(MemoryPoolTest, BasicAllocation) {
    MemoryPool pool(1024 * 1024);  // 1 MB пул
    auto [ptr1, size1] = pool.allocate(100);
    ASSERT_NE(ptr1, nullptr);
    ASSERT_GE(size1, 100);

    auto [ptr2, size2] = pool.allocate(200);
    ASSERT_NE(ptr2, nullptr);
    ASSERT_GE(size2, 200);

    pool.deallocate(ptr1, size1);
    pool.deallocate(ptr2, size2);
}

TEST(MemoryPoolTest, Defragmentation) {
    MemoryPool pool(1024 * 1024);
    auto [ptr1, size1] = pool.allocate(500);
    auto [ptr2, size2] = pool.allocate(500);

    pool.deallocate(ptr1, size1);
    pool.deallocate(ptr2, size2);

    pool.defrag();  // Проверяем, что не падает
}

TEST(MemoryPoolTest, DefragmentationPoolBeforeAndAfter) {
    MemoryPool pool(256 * 5, 256);
    auto [ptr1, size1] = pool.allocate(100);
    auto [ptr_tmp, size_tmp] = pool.allocate(100);
    auto [ptr2, size2] = pool.allocate(200);

    memset(ptr1, 0x22, size1);
    memset(ptr2, 0x33, size2);
    memset(ptr_tmp, 0xFF, size_tmp);

    auto old_ptr1 = ptr1;
    auto old_ptr2 = ptr2;

    pool.deallocate(ptr_tmp, size_tmp);

    pool.defrag([&](auto new_ptr, auto old_ptr) {
        if (new_ptr != old_ptr) {
            std::cout << "Moved to a new location\n";
        }
    });

    ASSERT_NE(ptr1, nullptr);
    ASSERT_NE(ptr2, nullptr);
}

/*TEST(MemoryPoolTest, DefragmentationUpdatesPointers) {
    MemoryPool pool(1024 * 1024);  // 1 MB
    auto [ptr1, size1] = pool.allocate(100);
    auto [ptr2, size2] = pool.allocate(200);

    // Заполняем данными
    memset(ptr1, 0xAA, 100);
    memset(ptr2, 0xBB, 200);

    // Хранилище для обновлённых указателей
    std::unordered_map<void*, void*> ptr_updates;

    // Захватываем указатели ДО дефрагментации
    void* old_ptr1 = ptr1;
    void* old_ptr2 = ptr2;

    // Дефрагментация с обновлением указателей
    pool.defrag([&](auto old_ptr, auto new_ptr) {
        ptr_updates[old_ptr] = new_ptr;
    });

    //auto ptr1_count = ptr_updates.count(old_ptr1);
    //auto ptr2_count = ptr_updates.count(old_ptr2);
    // Проверяем:
    // 1. Данные сохранились в новых адресах
    //ASSERT_NE(ptr1_count, 0);
    //ASSERT_NE(ptr2_count, 0);

    void* new_ptr1 = ptr_updates[old_ptr1];
    void* new_ptr2 = ptr_updates[old_ptr2];

    ASSERT_EQ(memcmp(new_ptr1, "\xAA\xAA\xAA", 3), 0);
    ASSERT_EQ(memcmp(new_ptr2, "\xBB\xBB\xBB", 3), 0);

    // 2. Старые указатели больше не валидны (но не используем их!)
    // 3. Новые указатели доступны для работы
    memset(new_ptr1, 0xFF, 10);  // Проверка записи
}*/