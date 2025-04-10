Высокопроизводительное хранилище VoidCache

1. Архитектура:
```text
┌───────────────────────────────────────────────────────┐
│                    Клиенты (UDP/XDP)                  │
└──────────────────────────┬────────────────────────────┘
                           │
┌──────────────────────────▼────────────────────────────┐
│              Шардированный кластер                    │
│  ┌──────────┐  ┌───────────┐  ┌──────────┐            │
│  │ Узел 1   │  │ Узел 2    │  │ Узел N   │            │
│  │ ┌───────┐│  │ ┌───────┐ │  │ ┌───────┐│            │
│  │ │Хэш-   ││  │ │Хэш-   │ │  │ │Хэш-   ││            │
│  │ │Таблица││  │ │Таблица│ │  │ │Таблица││            │
│  │ └───────┘│  │ └───────┘ │  │ └───────┘│            │
│  └──────────┘  └───────────┘  └──────────┘            │
└──────────────────────────┬────────────────────────────┘
                           │
┌──────────────────────────▼────────────────────────────┐
│              Фоновая служба сохранения                │
│  ┌────────────────┐  ┌────────────────┐               │
│  │   WAL (лог)    │  │  Снапшоты      │               │
│  └────────────────┘  └────────────────┘               │
└───────────────────────────────────────────────────────┘
```

3. Детализация компонентов
2.1. Memory Pool (блочный аллокатор)
Цель: Минимизировать фрагментацию и аллокации.
Алгоритм:

Память делится на блоки фиксированного размера (например, 4 KB).

Битмап отслеживает свободные блоки.

Аллокация:

```cpp
void* allocate(size_t size) {
    size_t blocks_needed = ceil(size / BLOCK_SIZE);
    size_t free_blocks = find_contiguous_blocks(blocks_needed); // Поиск в битмапе
    if (free_blocks == -1) trigger_defragmentation();
    mark_blocks_used(free_blocks, blocks_needed);
    return pool_start + free_blocks * BLOCK_SIZE;
}
```

Оптимизации:
Использование mmap с MAP_HUGETLB.
Выравнивание по cache-line (64 байта).

2.2. Lock-Free Hash Table (Robin Hood Hashing)
Цель: Бесконфликтный доступ при высокой нагрузке.
Алгоритм:
Вставка:
Линейный probing с перемещением "богатых" элементов (тех, у которых расстояние до исходной позиции больше).
CAS-операции для атомарного обновления:

```cpp
bool insert(K key, V value) {
    size_t pos = hash(key) % capacity;
    while (true) {
        Entry& entry = table[pos];
        if (entry.is_empty()) {
            if (CAS(&entry.state, FREE, LOCKED)) {
                entry.key = key; entry.value = value;
                entry.state = OCCUPIED;
                return true;
            }
        }
        pos = (pos + 1) % capacity;
    }
}
```

Чтение:
Чтение без блокировок (достаточно atomic-флагов).
Оптимизации:
QSBR (Quiescent State Based Reclamation) для безопасного удаления.

2.3. Дефрагментация памяти
Алгоритм:
Периодически (или при нехватке памяти) запускается сборщик мусора.
"Живые" данные копируются в новый регион памяти:

```cpp
void defragment() {
    char* new_region = malloc(capacity);
    size_t new_offset = 0;
    for (auto& entry : hash_table) {
        size_t size = entry.value_size;
        memcpy(new_region + new_offset, entry.value_ptr, size);
        entry.value_ptr = new_region + new_offset;
        new_offset += size;
    }
    free(old_region);
}
```

2.4. Сетевой стек (UDP + XDP)
Алгоритм:
XDP-программа:
Фильтрация пакетов на уровне ядра.
Перенаправление в userspace через AF_XDP:

```c
SEC("xdp") int xdp_cache_handler(struct xdp_md* ctx) {
    void* data = (void*)(long)ctx->data;
    if (!is_valid_packet(data)) return XDP_DROP;
    return bpf_redirect_map(&xsks_map, ctx->rx_queue_index, XDP_PASS);
}
```
Userspace-сервер:
Прием пакетов через recvmmsg (пакетный режим).

2.5. Горизонтальное масштабирование
Алгоритм:
Consistent Hashing:
Ключи распределяются по узлам через кольцо виртуальных нод.
Gossip-протокол:
Узлы обмениваются heartbeat-сообщениями.
При обнаружении мертвого узла его данные перераспределяются.

2.6. Сохранение состояния (WAL + Snapshots)
Алгоритм:
Write-Ahead Log:
Каждая операция пишется в лог перед исполнением:

```text
SET key_len key value_len value
DEL key_len key
```

Снапшоты:
Периодическое сохранение состояния в SSTable (как в LevelDB).

3. План тестирования
3.1. Юнит-тесты
Компонент	Тесты	Инструменты
Memory Pool	Аллокация/освобождение, фрагментация	Google Test
Lock-Free Hash Table	Многопоточные INSERT/GET	ThreadSanitizer
XDP	Пропускная способность	iperf3, perf
3.2. Нагрузочное тестирование
Утилиты:
wrk2 для генерации UDP-трафика.
grafana + prometheus для мониторинга.

Метрики:
RPS (запросов в секунду).
Latency (P50, P99).

gantt
    title План на 3 месяца
    dateFormat  YYYY-MM-DD
    section Ядро
    Memory Pool       :done, 2023-10-01, 14d
    Lock-Free Hash Table :active, 2023-10-15, 21d
    Дефрагментация     :2023-11-05, 14d
    section Сеть
    XDP Программа     :2023-11-19, 14d
    UDP-сервер        :2023-12-03, 7d
    section Распределенность
    Consistent Hashing :2023-12-10, 14d
    Gossip Protocol    :2023-12-24, 14d
