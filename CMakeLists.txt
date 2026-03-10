cmake_minimum_required(VERSION 3.20)
project(VoidCache LANGUAGES C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

# ── Performance flags ──────────────────────────────────────
add_compile_options(
    -O3
    -march=native
    -funroll-loops
    -fno-omit-frame-pointer
    -Wall -Wextra
)

# ── Shared library ─────────────────────────────────────────
include_directories(include)

add_library(voidcache STATIC src/voidcache.c)
target_include_directories(voidcache PUBLIC include)
target_link_libraries(voidcache pthread m)

# ── Tests ─────────────────────────────────────────────────
enable_testing()
add_executable(voidcache_test tests/test_voidcache.c)
target_link_libraries(voidcache_test voidcache)
add_test(NAME VoidCacheTests COMMAND voidcache_test)
set_tests_properties(VoidCacheTests PROPERTIES TIMEOUT 30)

# ── Benchmark ─────────────────────────────────────────────
add_executable(voidcache_bench bench/benchmark.c)
target_link_libraries(voidcache_bench voidcache m)
