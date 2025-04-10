#include "MemoryPool.h"
#include <cstring>
#include <stdexcept>
#include <sys/mman.h> // mprotect Linux API

MemoryPool::MemoryPool(size_t pool_size, size_t block_size)
    : pool_size(pool_size), block_size(block_size) {
    // Выделяем память с выравниванием по границе страницы
    memory = static_cast<char*>(aligned_alloc(block_size, pool_size)); // TODO: Попробовать заменить на mmap
    //memory = static_cast<char*>(mmap(nullptr, pool_size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0));

    if (!memory) throw std::bad_alloc();

    // Инициализируем битмап (все блоки свободны)
    block_map.resize(pool_size / block_size, true);
    free_blocks = block_map.size();

    // Защищаем память от записи (опционально)
    mprotect(memory, pool_size, PROT_READ | PROT_WRITE);
}

MemoryPool::~MemoryPool() {
    free(memory.load());
}

std::pair<void*, size_t> MemoryPool::allocate(size_t size) {
    size_t num_blocks = (size + block_size - 1) / block_size;  // Округление вверх
    size_t start_block = find_contiguous_blocks(num_blocks);

    if (start_block == static_cast<size_t>(-1)) {
        defragment();  // Попытка дефрагментации
        start_block = find_contiguous_blocks(num_blocks);
        if (start_block == static_cast<size_t>(-1)) return {nullptr, 0};
    }

    // Помечаем блоки как занятые
    for (size_t i = 0; i < num_blocks; ++i) {
        block_map[start_block + i] = false;
    }
    free_blocks -= num_blocks;

    char* ptr = memory.load() + start_block * block_size;
    return {ptr, num_blocks * block_size};
}

void MemoryPool::deallocate(void* ptr, size_t size) {
    size_t start_block = (static_cast<char*>(ptr) - memory.load()) / block_size;
    size_t num_blocks = (size + block_size - 1) / block_size;

    for (size_t i = 0; i < num_blocks; ++i) {
        block_map[start_block + i] = true;
    }
    free_blocks += num_blocks;
}

size_t MemoryPool::find_contiguous_blocks(size_t num_blocks) {
    for (size_t i = 0; i < block_map.size(); ) {
        if (block_map[i]) {
            size_t j = i;
            while (j < block_map.size() && block_map[j] && (j - i + 1) < num_blocks) {
                ++j;
            }
            if ((j - i + 1) >= num_blocks) return i;
            i = j + 1;
        } else {
            ++i;
        }
    }
    return -1;
}

void MemoryPool::defragment() {
    // Упрощенная дефрагментация: копируем все данные в новый регион
    char* new_memory = static_cast<char*>(aligned_alloc(block_size, pool_size));
    size_t new_offset = 0;

    // TODO: Реализовать логику копирования "живых" данных из memory в new_memory

    free(memory.load());
    memory.store(new_memory);
    std::fill(block_map.begin(), block_map.end(), true);
    free_blocks = block_map.size();
}