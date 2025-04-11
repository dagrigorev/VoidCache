cmake_minimum_required(VERSION 3.20)
project(VoidCache LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Оптимизация под производительность
#add_compile_options(-O3 -march=native)

# Включение тестов
enable_testing()
find_package(GTest REQUIRED)

include_directories(include)

# Исходные файлы
file(GLOB SOURCES "src/*.cpp")

# Сборка исполняемого файла
add_executable(void ${SOURCES})
# Сборка тестов
add_executable(memory_pool_test tests/MemoryPoolTests.cpp src/MemoryPool.cpp)

# Подключение заголовков
target_include_directories(void PUBLIC include)
target_link_libraries(memory_pool_test GTest::GTest GTest::Main)

add_test(NAME MemoryPoolTest COMMAND memory_pool_test)