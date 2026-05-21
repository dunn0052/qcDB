# Chunked Memory-Mapped I/O for qcDB

**Date:** 2026-05-21
**Status:** Approved for implementation

---

## Problem

`dbInterface` currently maps the entire database file into memory at open time using `MAP_POPULATE` (Linux), which pre-faults every page immediately. When multiple processes open the same database, each process accumulates resident pages for the full file even if only a small fraction of records are ever accessed. With multiple databases open simultaneously across many processes, total RSS grows proportionally to the sum of all file sizes — regardless of actual access.

Additionally, the current single read lock held for entire scan operations means writers must wait for a full scan to complete before they can write, causing write latency to scale with database size.

---

## Goals

- Reduce per-process RSS for large databases by mapping only the pages actually needed
- Allow writers to update records with minimal latency regardless of concurrent scan activity
- Prevent reader starvation under sustained writer load
- Require no API changes — `dbInterface(const std::string& dbPath)` constructor signature unchanged
- Support all platforms and page sizes dynamically

---

## Non-Goals

- User-configurable chunk size or threshold
- Per-record locking granularity
- Persistence or durability guarantees beyond what `MAP_SHARED` provides today

---

## Architecture

### Always-Windowed Strategy

All databases use chunked windowed mmap regardless of size. No threshold, no two-path branching.

For small databases where `total_records <= m_ChunkRecords`, the window covers the entire data region in a single mmap and never slides — behavior is identical to a full mmap but without `MAP_POPULATE`. The simplification eliminates an arbitrary size constant and one entire code branch.

- Header: permanent pinned mmap (`ceil(sizeof(DBHeader), m_PageSize)` bytes)
- Data: sliding chunk window; fd kept open for remapping

### File Layout (Unchanged)

```
[DBHeader][object_0][object_1]...[object_N]
```

`DBHeader` (containing `pthread_rwlock_t`) is always accessible via the pinned header mapping. All existing `reinterpret_cast<DBHeader*>(m_DBAddress)` calls remain valid.

---

## Dynamic Page Size

Page size is captured once at open and stored as `m_PageSize`:

```cpp
#ifdef WINDOWS_PLATFORM
    SYSTEM_INFO sysInfo;
    GetSystemInfo(&sysInfo);
    m_PageSize = sysInfo.dwAllocationGranularity;  // 65536 — required for MapViewOfFile offset alignment
#else
    m_PageSize = static_cast<size_t>(sysconf(_SC_PAGESIZE));  // typically 4096, but 65536 on some ARM64
#endif
```

> **Windows note:** `dwAllocationGranularity` (65536), not `dwPageSize` (4096), is the required alignment unit for `MapViewOfFile` offsets. Using `dwPageSize` for offset alignment will fail.

Chunk record count derived from page size at open time:

```cpp
m_ChunkRecords = std::max(size_t(1), m_PageSize / sizeof(object));
```

Examples:
- 32-byte struct, 4096-byte page → 128 records/chunk
- 512-byte struct, 4096-byte page → 8 records/chunk
- 8192-byte struct, 4096-byte page → 1 record/chunk (struct larger than page)
- 32-byte struct, 65536-byte page (Windows/ARM64) → 2048 records/chunk

---

## Window Management

### New Member Fields

```cpp
int    m_fd;                   // kept open for remapping
char*  m_DataWindow;           // current data chunk mmap base address
size_t m_WindowChunkIndex;     // which chunk is currently mapped
size_t m_WindowMappedBytes;    // total bytes currently mmap'd for data window
size_t m_WindowExtra;          // pad bytes from window base to first record in chunk
size_t m_ChunkRecords;         // records per chunk (derived from m_PageSize)
size_t m_PageSize;             // alignment granularity (platform-specific)
std::mutex m_WindowMutex;      // intra-process window protection (see Concurrency)
```

`m_DBAddress` retains its current meaning (pinned header address). `m_DataWindow` is the separate sliding data mapping.

### Page-Aligned Chunk Offset Calculation

```
chunk_index  = record / m_ChunkRecords
chunk_start  = chunk_index * m_ChunkRecords        // first record in this chunk
file_offset  = sizeof(DBHeader) + chunk_start * sizeof(object)
aligned_off  = (file_offset / m_PageSize) * m_PageSize    // floor to alignment boundary
extra        = file_offset - aligned_off            // pad bytes before first record
map_bytes    = ceil(extra + m_ChunkRecords * sizeof(object), m_PageSize)
```

Pointer to record `r` within a mapped chunk:

```
local_index = r - chunk_start
ptr         = window_base + extra + local_index * sizeof(object)
```

### `SlideWindow(size_t record)`

Called by `Get()` when record is outside the current window:

1. If `m_DataWindow` is valid: `munmap` / `UnmapViewOfFile` existing window
2. Compute `aligned_off`, `map_bytes`, `extra` for the chunk containing `record`
3. `mmap(aligned_off, map_bytes, MAP_SHARED)` / `MapViewOfFile`
4. Update `m_WindowChunkIndex`, `m_WindowMappedBytes`, `m_WindowExtra`, `m_DataWindow`
5. On failure: set `m_DataWindow = nullptr`, `m_IsOpen = false`, return error

Window is cached — remapping only occurs on chunk miss. Sequential access to records within the same chunk pays zero syscall overhead after the initial map.

### Header Mapping Size

The pinned header mmap covers `ceil(sizeof(DBHeader), m_PageSize)` bytes — one page on all known platforms since `sizeof(DBHeader)` is ~104 bytes. `m_DBAddress` points to this mapping and never changes.

### Destructor

Windowed path: `munmap(m_DataWindow)` + `close(m_fd)`. Header mapping always unmapped. Full-map path: unchanged.

---

## `Get()` Call Order Fix

Current bug in `ReadObject`, `WriteObject`, `DeleteObject`:

```cpp
char* p_object = Get(record);  // BEFORE lock — window can slide under pointer
LockDB();
memcpy(..., p_object, ...);    // pointer potentially invalidated
```

Fix: bounds-check before lock (no window access), slide window inside lock:

```cpp
// Fast bounds check — no window touch, no lock
if (!m_IsOpen || record >= m_NumRecords) return RTN_NULL_OBJ;

LockDB();                          // acquires file rwlock + m_WindowMutex (windowed mode)
char* p_object = Get(record);      // window managed here, under lock
if (nullptr == p_object) { UnlockDB(); return RTN_NULL_OBJ; }
memcpy(...);
UnlockDB();
```

---

## Bulk Operation Chunk Planning

`ReadObjects` and `WriteObjects` already sort records by index. After sorting, group records by chunk before mapping:

```
sorted records: [1, 2, 3, 7, 8, 1000, 1150]  (chunk_size = 128)

group by chunk_index = record / m_ChunkRecords:
  chunk 0  (records   0–127): [1, 2, 3, 7, 8]  → 1 mmap
  chunk 7  (records 896–1023): [1000]            → 1 mmap
  chunk 9  (records 1152–1279): [1150]            → 1 mmap

for each group:
    SlideWindow to chunk  (1 mmap syscall if miss)
    process all records in group within this window
    advance to next group
```

Total: 3 mmap operations instead of mapping the full file.

### Scan-for-Empty Write Overloads

`WriteObject(object&)` and `WriteObjects(vector<object>)` scan sequentially for empty records rather than targeting known indices. In windowed mode these hold the write lock for the entire scan (write lock is exclusive — no window mutex conflict) and call `SlideWindow` chunk-by-chunk as they advance. Because writers hold `m_LastWritten` in the header and start scanning from that point, the scan typically finds an empty slot within the first chunk and terminates early. Full-DB scan only occurs when the DB is nearly full.

---

## Scan Operations

### `FindFirstOf` — Sequential Chunk Walk

```
for chunk_index in 0..total_chunks:
    acquire read lock (blocking or trylock — see Concurrency)
    SlideWindow to chunk
    for each record in chunk:
        run predicate
        if match: store result, break
    release read lock
```

### `FindObjects` — Parallel Chunk Division

Threads never share `m_DataWindow`. Each thread receives its chunk range and opens its own local mmap for that range:

- Total chunk count divided among `numThreads`
- Thread function receives: `m_fd`, chunk range start offset, map size, page-size parameters
- Each thread: `mmap` its own mapping → scan records → `munmap` → push results → return
- Main thread: joins all threads, collects results (unchanged)

Thread function signature gains mmap parameters:

```cpp
static void FinderThread(Predicate predicate,
                         int fd,
                         size_t file_offset,   // page-aligned start
                         size_t map_bytes,
                         size_t extra,          // pad to first record
                         size_t num_records,
                         std::vector<object>& results);
```

All `FindObjects` calls use this thread signature — no separate full-map variant.

---

## Writer-Priority Concurrency

### Writer-Preference rwlock

The `pthread_rwlock_t` in `DBHeader` is initialized in dbGenerator (not `dbInterface`) with writer-preference attribute:

```cpp
pthread_rwlockattr_t attr;
pthread_rwlockattr_init(&attr);
pthread_rwlockattr_setkind_np(&attr, PTHREAD_RWLOCK_PREFER_WRITER_NONRECURSIVE_NP);
pthread_rwlock_init(&header->m_DBLock, &attr);
pthread_rwlockattr_destroy(&attr);
```

Effect: once a writer calls `wrlock()`, all subsequent `rdlock()` calls block until the writer finishes. The writer is reserved as next-in-line at the moment it calls `wrlock()`. Multiple writers queue naturally. No additional fields in `DBHeader` required.

> **Windows:** `SRWLOCK` has no native writer-preference mode. Windows implementation flagged as a separate concern — may require a manual writer-preference wrapper or fall back to exclusive mutex (losing read concurrency).

> **Non-Linux POSIX:** `PTHREAD_RWLOCK_PREFER_WRITER_NONRECURSIVE_NP` is Linux-specific. On other POSIX platforms, fall back to default rwlock attribute at dbGenerator init time. Writer preference lost but correctness preserved.

### Per-Chunk Lock Granularity in Scans

Scans acquire and release the read lock **once per chunk**, not once per entire scan. This bounds maximum writer wait time to one chunk scan duration:

```
← writer calls wrlock() here (reserved, waiting)
reader finishes current chunk → rdunlock()
writer acquires write lock → modifies record → wrlock released
reader acquires next chunk rdlock → continues scan
```

### Reader Skip + Anti-Starvation

On each chunk, readers use `tryrdlock()` (non-blocking). Failure means a writer is pending — reader skips the chunk (catches it next scan cycle, acceptable per use case).

Anti-starvation: after `READER_MAX_SKIP` (= 5, internal constant) consecutive skipped chunks, reader falls through to blocking `rdlock()`. Writers still receive broad priority (5 chunks leeway) but readers cannot be starved indefinitely.

```cpp
static constexpr size_t READER_MAX_SKIP = 5;

size_t consecutive_skips = 0;
for each chunk:
    if (consecutive_skips < READER_MAX_SKIP):
        if tryrdlock() fails:
            ++consecutive_skips
            skip chunk; continue
        consecutive_skips = 0
    else:
        rdlock()   // blocking
        consecutive_skips = 0

    SlideWindow; process chunk records
    rdunlock()
```

Each `FindObjects` thread maintains its own `consecutive_skips` counter independently.

### Intra-Process Window Safety

The file rwlock is process-shared and allows multiple threads in the same process to hold read locks simultaneously. Without protection, two threads could concurrently slide `m_DataWindow` — race condition.

Fix: `LockDB()` and `WriteLockDB()` also acquire `m_WindowMutex` (a `std::mutex` member) after acquiring the file rwlock, in windowed mode only. `UnlockDB()` releases `m_WindowMutex` before releasing the file rwlock. `Get()` is then always called while holding both.

Effect: single-record ops serialize within a process. Acceptable — the target workload bottleneck is cross-process, not intra-thread. `FindObjects` parallel threads are unaffected (thread-local mmaps, no shared window).

### Concurrency Scenario Summary

| Scenario | Behavior |
|---|---|
| Many readers scanning, no writer | All chunks processed; per-chunk lock release allows writer interleaving |
| Writer arrives mid-scan | Gets in after current chunk finishes (≤ 1 chunk wait) |
| Many writers, readers scanning | Readers skip up to 5 chunks, then force through blocking rdlock |
| Multiple writers queued | Serialize naturally via writer-preference rwlock |
| Single-record read + write contention | wrlock wins per writer-preference |
| Multiple processes, any mix | Cross-process: existing rwlock + MAP_SHARED coherence — unchanged |

---

## Error Handling

| Failure | Behavior |
|---|---|
| `SlideWindow` mmap fails | `m_DataWindow = nullptr`; `m_IsOpen = false`; return `RTN_NULL_OBJ` — instance must be re-opened |
| `munmap` fails in `SlideWindow` | Continue; memory leak but not a correctness failure; proceed with new mmap |
| `m_fd` closed unexpectedly | `SlideWindow` detects `m_fd == INVALID_FD`; return `RTN_NULL_OBJ` |
| `tryrdlock` fails in scan | Skip chunk; increment `consecutive_skips` — expected, not an error |
| Blocking `rdlock` fails after max skips | Return `RTN_LOCK_ERROR` — same as today |
| `FindObjects` thread-local mmap fails | Thread pushes nothing to results; main thread returns `RTN_OK` with partial results — consistent with existing behavior |
No new `RETCODE` values required. Windowed mmap failure surfaces as `RTN_NULL_OBJ`.

---

## Files Affected

| File | Change |
|---|---|
| `qcDB/qcDB.hh` | Replace full mmap with pinned header + sliding chunk window; add `SlideWindow()`; fix `Get()` call order; chunk-grouped bulk ops; per-chunk scan lock; `m_WindowMutex` in `LockDB`/`UnlockDB`/`WriteLockDB`; updated `FinderThread` signature |
| `common/DBHeader.hh` | No struct changes |
| `dbGenerator/` | Init rwlock with `PTHREAD_RWLOCK_PREFER_WRITER_NONRECURSIVE_NP` on Linux |
