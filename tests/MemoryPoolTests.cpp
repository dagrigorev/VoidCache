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

    pool.defragment();  // Проверяем, что не падает
}