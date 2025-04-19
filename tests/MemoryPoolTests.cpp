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
