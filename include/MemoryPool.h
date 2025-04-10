#pragma once
#include <vector>
#include <atomic>
#include <cstddef>

/**
 * Пул памяти
 * Необходим для ухода от постоянных выделений памяти стандартными механизмами
 */
class MemoryPool {
public:
    explicit MemoryPool(size_t pool_size, size_t block_size = 4096);
    ~MemoryPool() {}

    /**
     * Выделение памяти
     * Возвращает указатель и фактический размер выделенной памяти
     */
    std::pair<void*, size_t> allocate(size_t size);

    /**
     * Освобождает память
     * Отмечает блок как свободный
     */
    void deallocate(void* ptr, size_t size);

    /**
     * Дефрагментирует память
     */
    void defrag();

private:
    // Общий размер пула
    size_t pool_size;
    // Размер одного блока
    size_t block_size;
    // Картма памяти свободных блоков
    std::vector<bool> block_map;
    // TODO: Нахер векторы даешь обычный массив
    // std::atomic<uint64_t>[]
    
    // Указатель на память пула
    std::atomic<char*> memory;
    // TODO: Проверить использовать выравнивание через alignas 
    /*
    constexpr size_t CACHE_LINE_SIZE = 64;
    alignas(CACHE_LINE_SIZE) char memory[pool_size];
    */

    // Количество свободных блоков
    std::atomic<size_t> free_blocks;

    // Находит последовательно свободные блоки
    size_t find_contiguous_blocks(size_t num_blocks);
};