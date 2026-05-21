# Chunked Memory-Mapped I/O Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Replace full-file `mmap` in `dbInterface` with an always-windowed, page-aligned chunked mmap strategy that reduces per-process RSS and gives writers priority over concurrent readers.

**Architecture:** Pinned header mapping (`m_DBAddress`, one page, permanent) plus a sliding data window (`m_DataWindow`, one chunk, remapped on miss). `Get()` manages the window transparently; all public ops call `Get()` inside the lock. Scans release and reacquire the lock per chunk so writers can preempt between chunks.

**Tech Stack:** C++17, pthreads rwlock, POSIX mmap/munmap, `std::mutex`, `std::thread`. Linux primary; Windows path left structurally intact but writer-preference attribute is Linux-only.

**Spec:** `docs/superpowers/specs/2026-05-21-chunked-mmap-design.md`

---

## File Map

| File | Action | Responsibility |
|---|---|---|
| `qcDB/qcDB.hh` | Modify | All windowed mmap logic — only file changed for the interface |
| `dbGenerator/src/Schema.cpp` | Modify | Add writer-preference rwlock attribute at DB creation time |
| `unitTest/CMakeLists.txt` | Create | Build config for unit test executable |
| `unitTest/src/main.cpp` | Create | Correctness tests: cross-chunk access, bulk ops, scan, delete, clear |
| `CMakeLists.txt` | Modify | Wire in `add_subdirectory(unitTest)` |

---

## Task 1: Unit Test Infrastructure

**Files:**
- Create: `unitTest/CMakeLists.txt`
- Create: `unitTest/src/main.cpp`
- Modify: `CMakeLists.txt`

- [ ] **Step 1: Wire unitTest into root CMakeLists.txt**

In `/home/kdunn/qcDB/CMakeLists.txt`, add after the existing `add_subdirectory` lines:

```cmake
set(COMPONENT_UNIT_TEST unitTest)
add_subdirectory(${COMPONENT_UNIT_TEST})
```

- [ ] **Step 2: Create `unitTest/CMakeLists.txt`**

```cmake
cmake_minimum_required(VERSION 3.21)
project(${COMPONENT_UNIT_TEST})

set(SRC
    src/main.cpp
)

add_executable(${PROJECT_NAME}
    ${SRC}
)

set(THREADS_PREFER_PTHREAD_FLAG ON)
find_package(Threads REQUIRED)
target_link_libraries(${PROJECT_NAME} PRIVATE Threads::Threads)

target_include_directories(${PROJECT_NAME} PRIVATE
    ${CMAKE_SOURCE_DIR}
)
```

- [ ] **Step 3: Create `unitTest/src/main.cpp` with test struct, DB creation helper, and harness**

```cpp
#include <qcDB/qcDB.hh>
#include <common/DBHeader.hh>

#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <unistd.h>
#include <pthread.h>
#include <sys/stat.h>

// 32-byte struct — on 4KB page, 128 records/chunk; on 64KB page, 2048/chunk
struct TestRecord {
    int id;
    char name[28];
};
static_assert(sizeof(TestRecord) == 32, "TestRecord must be 32 bytes");

#define TEST_ASSERT(cond, msg) \
    do { if (!(cond)) { fprintf(stderr, "FAIL [%s:%d]: %s\n", __FILE__, __LINE__, msg); return false; } } while(0)

static bool CreateTestDB(const char* path, size_t num_records)
{
    size_t fileSize = sizeof(DBHeader) + num_records * sizeof(TestRecord);
    int fd = open(path, O_RDWR | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) return false;

    if (ftruncate(fd, static_cast<off_t>(fileSize)) != 0) { close(fd); return false; }

    DBHeader header = {};
    header.m_NumRecords = num_records;
    strncpy(header.m_ObjectName, "TEST", sizeof(header.m_ObjectName) - 1);

    pthread_rwlockattr_t attr = {};
    pthread_rwlockattr_init(&attr);
    pthread_rwlockattr_setpshared(&attr, PTHREAD_PROCESS_SHARED);
#ifndef WINDOWS_PLATFORM
    pthread_rwlockattr_setkind_np(&attr, PTHREAD_RWLOCK_PREFER_WRITER_NONRECURSIVE_NP);
#endif
    pthread_rwlock_init(&header.m_DBLock, &attr);
    pthread_rwlockattr_destroy(&attr);

    ssize_t written = write(fd, &header, sizeof(DBHeader));
    close(fd);
    return written == static_cast<ssize_t>(sizeof(DBHeader));
}

// ---- Tests declared here, defined after ----
static bool TestSingleRecordWriteRead(const char* path);
static bool TestCrossChunkAccess(const char* path);
static bool TestBulkReadObjects(const char* path);
static bool TestFindFirstOf(const char* path);
static bool TestDeleteObject(const char* path);
static bool TestClear(const char* path);

int main()
{
    const char* dbPath = "/tmp/test_chunk_qcdb.db";
    // 300 records: 3 chunks on 4KB page (128/chunk), or fewer larger chunks on bigger pages
    if (!CreateTestDB(dbPath, 300))
    {
        fprintf(stderr, "FAIL: could not create test DB\n");
        return 1;
    }

    bool pass = true;
    pass &= TestSingleRecordWriteRead(dbPath);
    pass &= TestCrossChunkAccess(dbPath);
    pass &= TestBulkReadObjects(dbPath);
    pass &= TestFindFirstOf(dbPath);
    pass &= TestDeleteObject(dbPath);
    pass &= TestClear(dbPath);

    unlink(dbPath);
    fprintf(stdout, pass ? "ALL TESTS PASSED\n" : "SOME TESTS FAILED\n");
    return pass ? 0 : 1;
}

static bool TestSingleRecordWriteRead(const char* path)
{
    qcDB::dbInterface<TestRecord> db(path);
    TEST_ASSERT(db.NumberOfRecords() == 300, "NumberOfRecords should be 300");

    TestRecord w = {};
    w.id = 42;
    strncpy(w.name, "ALPHA", sizeof(w.name) - 1);
    TEST_ASSERT(RTN_OK == db.WriteObject(0, w), "WriteObject(0) failed");

    TestRecord r = {};
    TEST_ASSERT(RTN_OK == db.ReadObject(0, r), "ReadObject(0) failed");
    TEST_ASSERT(r.id == 42, "ReadObject(0) id mismatch");
    TEST_ASSERT(strcmp(r.name, "ALPHA") == 0, "ReadObject(0) name mismatch");

    fprintf(stdout, "PASS: TestSingleRecordWriteRead\n");
    return true;
}

static bool TestCrossChunkAccess(const char* path)
{
    qcDB::dbInterface<TestRecord> db(path);

    // Write records in different chunks
    TestRecord w0 = {}; w0.id = 10; strncpy(w0.name, "CHUNK0", 28);
    TestRecord w1 = {}; w1.id = 20; strncpy(w1.name, "CHUNK1", 28);
    TestRecord w2 = {}; w2.id = 30; strncpy(w2.name, "CHUNK2", 28);

    // Record 5 = chunk 0, record 200 = chunk 1 (if chunk=128), record 290 = chunk 2
    // Works for any chunk size >= 1
    TEST_ASSERT(RTN_OK == db.WriteObject(5, w0),   "WriteObject(5) failed");
    TEST_ASSERT(RTN_OK == db.WriteObject(200, w1), "WriteObject(200) failed");
    TEST_ASSERT(RTN_OK == db.WriteObject(290, w2), "WriteObject(290) failed");

    TestRecord r = {};
    TEST_ASSERT(RTN_OK == db.ReadObject(5, r),   "ReadObject(5) failed");
    TEST_ASSERT(r.id == 10, "ReadObject(5) id mismatch");

    TEST_ASSERT(RTN_OK == db.ReadObject(200, r), "ReadObject(200) failed");
    TEST_ASSERT(r.id == 20, "ReadObject(200) id mismatch");

    TEST_ASSERT(RTN_OK == db.ReadObject(290, r), "ReadObject(290) failed");
    TEST_ASSERT(r.id == 30, "ReadObject(290) id mismatch");

    fprintf(stdout, "PASS: TestCrossChunkAccess\n");
    return true;
}

static bool TestBulkReadObjects(const char* path)
{
    qcDB::dbInterface<TestRecord> db(path);

    TestRecord w50 = {}; w50.id = 50; strncpy(w50.name, "FIFTY",   28);
    TestRecord w150 = {}; w150.id = 150; strncpy(w150.name, "ONEFIFTY", 28);
    TestRecord w280 = {}; w280.id = 280; strncpy(w280.name, "TWHEIGHTY", 28);
    TEST_ASSERT(RTN_OK == db.WriteObject(50,  w50),  "WriteObject(50) failed");
    TEST_ASSERT(RTN_OK == db.WriteObject(150, w150), "WriteObject(150) failed");
    TEST_ASSERT(RTN_OK == db.WriteObject(280, w280), "WriteObject(280) failed");

    // ReadObjects in unsorted order — should sort internally
    TestRecord empty = {};
    std::vector<std::tuple<size_t, TestRecord>> reads = {
        {280, empty}, {50, empty}, {150, empty}
    };
    TEST_ASSERT(RTN_OK == db.ReadObjects(reads), "ReadObjects failed");

    for (auto& t : reads)
    {
        size_t rec = std::get<0>(t);
        int gotId = std::get<1>(t).id;
        if (rec == 50)  TEST_ASSERT(gotId == 50,  "ReadObjects record 50 id wrong");
        if (rec == 150) TEST_ASSERT(gotId == 150, "ReadObjects record 150 id wrong");
        if (rec == 280) TEST_ASSERT(gotId == 280, "ReadObjects record 280 id wrong");
    }

    fprintf(stdout, "PASS: TestBulkReadObjects\n");
    return true;
}

static bool TestFindFirstOf(const char* path)
{
    qcDB::dbInterface<TestRecord> db(path);

    TestRecord w = {}; w.id = 9999; strncpy(w.name, "NEEDLE", 28);
    TEST_ASSERT(RTN_OK == db.WriteObject(175, w), "WriteObject(175) failed");

    size_t foundRecord = 0;
    RETCODE rc = db.FindFirstOf(
        [](const TestRecord* r) { return r->id == 9999; },
        foundRecord);
    TEST_ASSERT(RTN_OK == rc, "FindFirstOf failed");
    TEST_ASSERT(foundRecord == 175, "FindFirstOf returned wrong record");

    fprintf(stdout, "PASS: TestFindFirstOf\n");
    return true;
}

static bool TestDeleteObject(const char* path)
{
    qcDB::dbInterface<TestRecord> db(path);

    TestRecord w = {}; w.id = 77; strncpy(w.name, "DEL", 28);
    TEST_ASSERT(RTN_OK == db.WriteObject(250, w), "WriteObject(250) failed");

    TEST_ASSERT(RTN_OK == db.DeleteObject(250), "DeleteObject(250) failed");

    TestRecord r = {};
    TEST_ASSERT(RTN_OK == db.ReadObject(250, r), "ReadObject(250) after delete failed");
    TestRecord zero = {};
    TEST_ASSERT(memcmp(&r, &zero, sizeof(TestRecord)) == 0, "Record 250 not zeroed after delete");

    fprintf(stdout, "PASS: TestDeleteObject\n");
    return true;
}

static bool TestClear(const char* path)
{
    qcDB::dbInterface<TestRecord> db(path);

    TestRecord w = {}; w.id = 1; strncpy(w.name, "KEEP", 28);
    TEST_ASSERT(RTN_OK == db.WriteObject(10, w), "WriteObject before Clear failed");

    TEST_ASSERT(RTN_OK == db.Clear(), "Clear failed");

    TestRecord r = {};
    TEST_ASSERT(RTN_OK == db.ReadObject(10, r), "ReadObject after Clear failed");
    TestRecord zero = {};
    TEST_ASSERT(memcmp(&r, &zero, sizeof(TestRecord)) == 0, "Record 10 not zeroed after Clear");

    fprintf(stdout, "PASS: TestClear\n");
    return true;
}
```

- [ ] **Step 4: Build and verify compilation**

```bash
cd /home/kdunn/qcDB && cmake . -DCMAKE_BUILD_TYPE=Release 2>&1 | tail -5 && make unitTest -j$(nproc) 2>&1
```

Expected: compiles successfully. Tests will run against current (non-windowed) implementation and should pass — they establish the correctness baseline.

- [ ] **Step 5: Run tests against current implementation to establish baseline**

```bash
/home/kdunn/qcDB/bin/unitTest
```

Expected output:
```
PASS: TestSingleRecordWriteRead
PASS: TestCrossChunkAccess
PASS: TestBulkReadObjects
PASS: TestFindFirstOf
PASS: TestDeleteObject
PASS: TestClear
ALL TESTS PASSED
```

- [ ] **Step 6: Commit**

```bash
git -C /home/kdunn/qcDB add unitTest/ CMakeLists.txt
git -C /home/kdunn/qcDB commit -m "test: add unit test infrastructure for chunked mmap"
```

---

## Task 2: New Member Fields and Includes

**Files:**
- Modify: `qcDB/qcDB.hh`

- [ ] **Step 1: Add `#include <mutex>` and `#include <climits>` at top of `qcDB.hh`**

After the existing `#include <functional>` line (around line 17), add:

```cpp
#include <mutex>
#include <climits>
```

- [ ] **Step 2: Add new member fields and constants at bottom of class (replacing current member block)**

Replace the existing member block (lines 764–775):

```cpp
    bool m_IsOpen;
    size_t m_Size;
    size_t m_NumRecords;
    char* m_DBAddress;

#ifdef WINDOWS_PLATFORM
    HANDLE m_Mutex;
#else
#endif

    static constexpr int INVALID_FD = -1;
```

With:

```cpp
    bool   m_IsOpen;
    size_t m_Size;           // total file size in bytes
    size_t m_NumRecords;
    char*  m_DBAddress;      // pinned header mapping (one page, permanent)

    // Windowed data mapping
    int    m_fd;             // kept open for remapping; INVALID_FD on close
    char*  m_DataWindow;     // current data chunk mmap base
    size_t m_WindowChunkIndex;   // which chunk is currently mapped
    size_t m_WindowMappedBytes;  // bytes currently mmap'd for data window
    size_t m_WindowExtra;        // pad bytes from window base to first record in chunk
    size_t m_ChunkRecords;       // records per chunk (derived from m_PageSize)
    size_t m_PageSize;           // OS alignment granularity

    std::mutex m_WindowMutex;    // serializes window slides within a process

#ifdef WINDOWS_PLATFORM
    HANDLE m_Mutex;
#endif

    static constexpr int    INVALID_FD       = -1;
    static constexpr size_t READER_MAX_SKIP  = 5;
```

- [ ] **Step 3: Build to confirm no compilation errors**

```bash
cd /home/kdunn/qcDB && make unitTest -j$(nproc) 2>&1
```

Expected: compiles without errors.

- [ ] **Step 4: Run tests to confirm no regression**

```bash
/home/kdunn/qcDB/bin/unitTest
```

Expected: `ALL TESTS PASSED`

- [ ] **Step 5: Commit**

```bash
git -C /home/kdunn/qcDB add qcDB/qcDB.hh
git -C /home/kdunn/qcDB commit -m "feat: add windowed mmap member fields and constants to dbInterface"
```

---

## Task 3: Constructor Rewrite

**Files:**
- Modify: `qcDB/qcDB.hh`

Replace the entire constructor body with the new windowed approach. The constructor must: capture page size, open fd (keep open), mmap just the header page, compute chunk size, and map the initial data window.

- [ ] **Step 1: Replace the Linux constructor body**

Replace the constructor (currently lines 539–632) with:

```cpp
        dbInterface(const std::string& dbPath) :
            m_IsOpen(false), m_Size(0),
            m_DBAddress(nullptr), m_NumRecords(0),
            m_fd(INVALID_FD), m_DataWindow(nullptr),
            m_WindowChunkIndex(SIZE_MAX), m_WindowMappedBytes(0),
            m_WindowExtra(0), m_ChunkRecords(1), m_PageSize(4096)
#ifdef WINDOWS_PLATFORM
            , m_Mutex(INVALID_HANDLE_VALUE)
#endif
        {
#ifdef WINDOWS_PLATFORM
            SYSTEM_INFO sysInfo;
            GetSystemInfo(&sysInfo);
            m_PageSize = static_cast<size_t>(sysInfo.dwAllocationGranularity);

            HANDLE hFile = CreateFileA(
                static_cast<LPCSTR>(dbPath.c_str()),
                GENERIC_READ | GENERIC_WRITE,
                FILE_SHARE_READ | FILE_SHARE_WRITE,
                NULL,
                OPEN_EXISTING,
                FILE_ATTRIBUTE_NORMAL,
                NULL
            );

            if (hFile == INVALID_HANDLE_VALUE)
                return;

            m_Size = static_cast<size_t>(GetFileSize(hFile, NULL));
            if (m_Size == INVALID_FILE_SIZE)
            {
                CloseHandle(hFile);
                return;
            }

            // Map header page permanently
            size_t headerMapSize = m_PageSize;
            HANDLE hMapFile = CreateFileMappingA(hFile, NULL, PAGE_READWRITE, 0,
                static_cast<DWORD>(headerMapSize), NULL);
            if (hMapFile == NULL)
            {
                CloseHandle(hFile);
                return;
            }

            m_DBAddress = static_cast<char*>(MapViewOfFile(hMapFile, FILE_MAP_ALL_ACCESS,
                0, 0, headerMapSize));
            CloseHandle(hMapFile);
            CloseHandle(hFile);

            if (nullptr == m_DBAddress)
                return;

#else
            m_PageSize = static_cast<size_t>(sysconf(_SC_PAGESIZE));

            m_fd = open(dbPath.c_str(), O_RDWR);
            if (m_fd < 0)
            {
                m_fd = INVALID_FD;
                return;
            }

            struct stat statbuf;
            if (0 > fstat(m_fd, &statbuf))
            {
                close(m_fd);
                m_fd = INVALID_FD;
                return;
            }
            m_Size = static_cast<size_t>(statbuf.st_size);

            // Map header permanently — one page covers sizeof(DBHeader) (~104 bytes)
            size_t headerMapSize = m_PageSize;
            m_DBAddress = static_cast<char*>(mmap(nullptr, headerMapSize,
                PROT_READ | PROT_WRITE, MAP_SHARED, m_fd, 0));
            if (MAP_FAILED == m_DBAddress)
            {
                m_DBAddress = nullptr;
                close(m_fd);
                m_fd = INVALID_FD;
                return;
            }
#endif
            m_NumRecords = reinterpret_cast<DBHeader*>(m_DBAddress)->m_NumRecords;
            m_ChunkRecords = std::max(size_t(1), m_PageSize / sizeof(object));

            // Map initial data window (chunk 0) if DB has records
            if (m_NumRecords > 0 && !SlideWindow(0))
            {
#ifdef WINDOWS_PLATFORM
                UnmapViewOfFile(m_DBAddress);
#else
                munmap(m_DBAddress, m_PageSize);
                close(m_fd);
                m_fd = INVALID_FD;
#endif
                m_DBAddress = nullptr;
                return;
            }

            m_IsOpen = true;
        }
```

- [ ] **Step 2: Build**

```bash
cd /home/kdunn/qcDB && make unitTest -j$(nproc) 2>&1
```

Expected: `SlideWindow` is undefined — link error expected at this step (it's declared in step but not yet defined). This is normal; `SlideWindow` is added in Task 4.

- [ ] **Step 3: Commit constructor skeleton**

```bash
git -C /home/kdunn/qcDB add qcDB/qcDB.hh
git -C /home/kdunn/qcDB commit -m "feat: rewrite dbInterface constructor for pinned header + windowed data mmap"
```

---

## Task 4: `SlideWindow()` and Rewrite `Get()`

**Files:**
- Modify: `qcDB/qcDB.hh`

`SlideWindow` computes the page-aligned file offset for the chunk containing `record`, unmaps the old window, and maps the new one. `Get()` uses the window instead of `m_DBAddress`.

- [ ] **Step 1: Add `SlideWindow()` to the `protected` section (before `Get()`)**

```cpp
    /*
     * Remap the data window to the chunk containing `record`.
     * Returns false and sets m_IsOpen=false on mmap failure.
     * Must be called while m_WindowMutex is held.
     */
    bool SlideWindow(size_t record)
    {
#ifdef WINDOWS_PLATFORM
        if (nullptr != m_DataWindow)
        {
            UnmapViewOfFile(m_DataWindow);
            m_DataWindow = nullptr;
        }
#else
        if (nullptr != m_DataWindow)
        {
            munmap(m_DataWindow, m_WindowMappedBytes);
            m_DataWindow = nullptr;
        }
#endif
        size_t chunk_index  = record / m_ChunkRecords;
        size_t chunk_start  = chunk_index * m_ChunkRecords;
        size_t file_offset  = sizeof(DBHeader) + chunk_start * sizeof(object);
        size_t aligned_off  = (file_offset / m_PageSize) * m_PageSize;
        size_t extra        = file_offset - aligned_off;
        size_t data_bytes   = m_ChunkRecords * sizeof(object);
        size_t map_bytes    = ((extra + data_bytes + m_PageSize - 1) / m_PageSize) * m_PageSize;

        if (aligned_off + map_bytes > m_Size)
            map_bytes = m_Size - aligned_off;

#ifdef WINDOWS_PLATFORM
        HANDLE hMapFile = OpenFileMappingA(FILE_MAP_ALL_ACCESS, FALSE, NULL);
        // Windows path: use CreateFileMapping on stored handle (simplified — see note)
        // Full Windows windowed implementation requires storing the file HANDLE.
        // For now, set m_IsOpen = false on this path.
        m_IsOpen = false;
        return false;
#else
        char* window = static_cast<char*>(mmap(nullptr, map_bytes,
            PROT_READ | PROT_WRITE, MAP_SHARED, m_fd,
            static_cast<off_t>(aligned_off)));

        if (MAP_FAILED == window)
        {
            m_DataWindow = nullptr;
            m_IsOpen = false;
            return false;
        }
#endif
        m_DataWindow        = window;
        m_WindowChunkIndex  = chunk_index;
        m_WindowMappedBytes = map_bytes;
        m_WindowExtra       = extra;
        return true;
    }
```

- [ ] **Step 2: Replace `Get()` (currently lines 726–746)**

```cpp
    /*
     * Return pointer to record's data within the current window.
     * Slides window if record is in a different chunk.
     * Must be called while m_WindowMutex is held.
     */
    char* Get(const size_t record)
    {
        if (!m_IsOpen || nullptr == m_DBAddress || record >= m_NumRecords)
            return nullptr;

        size_t chunk_index = record / m_ChunkRecords;
        if (chunk_index != m_WindowChunkIndex || nullptr == m_DataWindow)
        {
            if (!SlideWindow(record))
                return nullptr;
        }

        size_t chunk_start = m_WindowChunkIndex * m_ChunkRecords;
        return m_DataWindow + m_WindowExtra + (record - chunk_start) * sizeof(object);
    }
```

- [ ] **Step 3: Build**

```bash
cd /home/kdunn/qcDB && make unitTest -j$(nproc) 2>&1
```

Expected: compiles. Tests may fail because lock functions don't yet acquire `m_WindowMutex` and `Get()` call order isn't fixed yet.

- [ ] **Step 4: Run tests (expect some may fail — establishing intermediate state)**

```bash
/home/kdunn/qcDB/bin/unitTest
```

- [ ] **Step 5: Commit**

```bash
git -C /home/kdunn/qcDB add qcDB/qcDB.hh
git -C /home/kdunn/qcDB commit -m "feat: add SlideWindow() and rewrite Get() for windowed mmap"
```

---

## Task 5: Lock Functions and Fix `Get()` Call Order

**Files:**
- Modify: `qcDB/qcDB.hh`

`LockDB` and `WriteLockDB` acquire `m_WindowMutex` after the file rwlock. `UnlockDB` releases `m_WindowMutex` before releasing the file lock. `ReadObject`, `WriteObject(size_t,object&)`, and `DeleteObject` all have the bug of calling `Get()` before the lock — fix all three.

- [ ] **Step 1: Replace `LockDB()`, `WriteLockDB()`, `UnlockDB()` (currently lines 654–720)**

```cpp
    RETCODE LockDB(void)
    {
#ifdef WINDOWS_PLATFORM
        m_Mutex = CreateMutexA(NULL, FALSE, "MutexForFileLock");
        if (nullptr == m_Mutex)
            return RTN_LOCK_ERROR;
        if (WAIT_FAILED == WaitForSingleObject(m_Mutex, INFINITE))
            return RTN_LOCK_ERROR;
#else
        int lockError = pthread_rwlock_rdlock(&reinterpret_cast<DBHeader*>(m_DBAddress)->m_DBLock);
        if (0 != lockError)
            return RTN_LOCK_ERROR;
#endif
        m_WindowMutex.lock();
        return RTN_OK;
    }

    RETCODE UnlockDB(void)
    {
        m_WindowMutex.unlock();
#ifdef WINDOWS_PLATFORM
        if (!ReleaseMutex(m_Mutex))
            return RTN_LOCK_ERROR;
#else
        int lockError = pthread_rwlock_unlock(&reinterpret_cast<DBHeader*>(m_DBAddress)->m_DBLock);
        if (0 != lockError)
            return RTN_LOCK_ERROR;
#endif
        return RTN_OK;
    }

    RETCODE WriteLockDB(void)
    {
#ifdef WINDOWS_PLATFORM
        m_Mutex = CreateMutexA(NULL, FALSE, "MutexForFileLock");
        if (nullptr == m_Mutex)
            return RTN_LOCK_ERROR;
        if (WAIT_FAILED == WaitForSingleObject(m_Mutex, INFINITE))
            return RTN_LOCK_ERROR;
#else
        int lockError = pthread_rwlock_wrlock(&reinterpret_cast<DBHeader*>(m_DBAddress)->m_DBLock);
        if (0 != lockError)
            return RTN_LOCK_ERROR;
#endif
        m_WindowMutex.lock();
        return RTN_OK;
    }
```

- [ ] **Step 2: Replace `ReadObject()` — move `Get()` inside lock**

Replace the current `ReadObject` (lines 36–60):

```cpp
        RETCODE ReadObject(size_t record, object& out_object)
        {
            if (!m_IsOpen || record >= m_NumRecords)
                return RTN_NULL_OBJ;

            RETCODE retcode = LockDB();
            if (RTN_OK != retcode)
                return retcode;

            char* p_object = Get(record);
            if (nullptr == p_object)
            {
                UnlockDB();
                return RTN_NULL_OBJ;
            }

            memcpy(&out_object, p_object, sizeof(object));
            return UnlockDB();
        }
```

- [ ] **Step 3: Replace `WriteObject(size_t record, object& objectWrite)` — move `Get()` inside lock**

Replace the first `WriteObject` overload (lines 106–137):

```cpp
        RETCODE WriteObject(size_t record, object& objectWrite)
        {
            if (!m_IsOpen || record >= m_NumRecords)
                return RTN_NULL_OBJ;

            RETCODE retcode = WriteLockDB();
            if (RTN_OK != retcode)
                return retcode;

            char* p_object = Get(record);
            if (nullptr == p_object)
            {
                UnlockDB();
                return RTN_NULL_OBJ;
            }

            DBHeader* header = reinterpret_cast<DBHeader*>(m_DBAddress);
            header->m_LastWritten = record;
            if (header->m_Size < record)
                header->m_Size = record;

            memcpy(p_object, &objectWrite, sizeof(object));
            return UnlockDB();
        }
```

- [ ] **Step 4: Replace `DeleteObject()` — move `Get()` inside lock, replace pointer walk with index walk**

Replace `DeleteObject` (lines 294–343):

```cpp
        RETCODE DeleteObject(size_t record)
        {
            if (!m_IsOpen || record >= m_NumRecords)
                return RTN_NULL_OBJ;

            RETCODE retcode = WriteLockDB();
            if (RTN_OK != retcode)
                return retcode;

            char* p_object = Get(record);
            if (nullptr == p_object)
            {
                UnlockDB();
                return RTN_NULL_OBJ;
            }

            DBHeader* header = reinterpret_cast<DBHeader*>(m_DBAddress);
            size_t size = header->m_Size;
            object deletedObject = {};

            memset(p_object, 0, sizeof(object));

            if (size == record)
            {
                // Walk backward by index to find new last non-empty record
                size_t newSize = 0;
                for (size_t i = record; i-- > 0;)
                {
                    char* p = Get(i);
                    if (nullptr == p) break;
                    if (memcmp(p, &deletedObject, sizeof(object)) != 0)
                    {
                        newSize = i;
                        break;
                    }
                }
                header->m_Size = newSize;
            }

            return UnlockDB();
        }
```

- [ ] **Step 5: Build**

```bash
cd /home/kdunn/qcDB && make unitTest -j$(nproc) 2>&1
```

Expected: compiles without errors.

- [ ] **Step 6: Run tests**

```bash
/home/kdunn/qcDB/bin/unitTest
```

Expected: `ALL TESTS PASSED`

- [ ] **Step 7: Commit**

```bash
git -C /home/kdunn/qcDB add qcDB/qcDB.hh
git -C /home/kdunn/qcDB commit -m "feat: acquire m_WindowMutex in lock functions; fix Get() call order in single-record ops"
```

---

## Task 6: Fix `Clear()` and Scan-for-Empty Write Overloads

**Files:**
- Modify: `qcDB/qcDB.hh`

`Clear()` currently does one `memset` over all data via `m_DBAddress` — broken in windowed mode. Replace with chunk-by-chunk zeroing via `SlideWindow` + `memset`. `WriteObject(object&)` and `WriteObjects(vector<object>)` both use raw pointer arithmetic to walk records for empty slots — replace with `Get(record)` calls.

- [ ] **Step 1: Replace `Clear()` (lines 348–378)**

```cpp
        RETCODE Clear(void)
        {
            if (!m_IsOpen)
                return RTN_MALLOC_FAIL;

            RETCODE retcode = WriteLockDB();
            if (RTN_OK != retcode)
                return retcode;

            size_t total_chunks = (m_NumRecords + m_ChunkRecords - 1) / m_ChunkRecords;
            for (size_t chunk = 0; chunk < total_chunks; chunk++)
            {
                size_t chunk_start = chunk * m_ChunkRecords;
                if (!SlideWindow(chunk_start))
                {
                    UnlockDB();
                    return RTN_NULL_OBJ;
                }
                size_t chunk_end = std::min(chunk_start + m_ChunkRecords, m_NumRecords);
                size_t records_in_chunk = chunk_end - chunk_start;
                memset(m_DataWindow + m_WindowExtra, 0, records_in_chunk * sizeof(object));
            }

            DBHeader* header = reinterpret_cast<DBHeader*>(m_DBAddress);
            header->m_LastWritten = 0;
            header->m_Size = 0;

            return UnlockDB();
        }
```

- [ ] **Step 2: Replace `WriteObject(object& objectWrite)` — the scan-for-empty overload (lines 142–185)**

```cpp
        RETCODE WriteObject(object& objectWrite)
        {
            const object emptyObject = {};
            RETCODE retcode = WriteLockDB();
            if (RTN_OK != retcode)
                return retcode;

            DBHeader* header = reinterpret_cast<DBHeader*>(m_DBAddress);
            size_t record = header->m_LastWritten;
            bool found = false;

            for (; record < m_NumRecords; record++)
            {
                char* p = Get(record);
                if (nullptr == p)
                {
                    UnlockDB();
                    return RTN_NULL_OBJ;
                }

                if (memcmp(p, &emptyObject, sizeof(object)) == 0)
                {
                    header->m_LastWritten = record;
                    memcpy(p, &objectWrite, sizeof(object));
                    if (header->m_Size < record)
                        header->m_Size = record;
                    found = true;
                    break;
                }
            }

            retcode = UnlockDB();
            if (RTN_OK != retcode)
                return retcode;

            return found ? RTN_OK : RTN_NOT_FOUND;
        }
```

- [ ] **Step 3: Replace `WriteObjects(std::vector<object>& objects)` — the scan-for-empty bulk overload (lines 239–289)**

```cpp
        RETCODE WriteObjects(std::vector<object>& objects)
        {
            const object emptyObject = {};
            auto objectsIterator = objects.begin();

            RETCODE retcode = WriteLockDB();
            if (RTN_OK != retcode)
                return retcode;

            DBHeader* header = reinterpret_cast<DBHeader*>(m_DBAddress);
            size_t record = header->m_LastWritten;

            for (; record < m_NumRecords; record++)
            {
                char* p = Get(record);
                if (nullptr == p)
                {
                    UnlockDB();
                    return RTN_NULL_OBJ;
                }

                if (0 == memcmp(p, &emptyObject, sizeof(object)))
                {
                    if (objectsIterator == objects.end())
                    {
                        header->m_LastWritten = record;
                        break;
                    }
                    memcpy(p, &(*objectsIterator), sizeof(object));
                    ++objectsIterator;
                }
            }

            if (header->m_Size < record)
                header->m_Size = record;

            retcode = UnlockDB();
            if (RTN_OK != retcode)
                return retcode;

            return (objectsIterator != objects.end()) ? RTN_EOF : RTN_OK;
        }
```

- [ ] **Step 4: Build and test**

```bash
cd /home/kdunn/qcDB && make unitTest -j$(nproc) 2>&1 && /home/kdunn/qcDB/bin/unitTest
```

Expected: `ALL TESTS PASSED`

- [ ] **Step 5: Commit**

```bash
git -C /home/kdunn/qcDB add qcDB/qcDB.hh
git -C /home/kdunn/qcDB commit -m "feat: fix Clear() and scan-for-empty write overloads for windowed mmap"
```

---

## Task 7: `FindFirstOf()` — Per-Chunk Lock and Reader Skip

**Files:**
- Modify: `qcDB/qcDB.hh`

Replace the single-lock full-scan with a per-chunk acquire/release cycle. On each chunk, attempt `pthread_rwlock_tryrdlock` first; skip chunk on failure; block after `READER_MAX_SKIP` consecutive skips.

- [ ] **Step 1: Replace `FindFirstOf()` (lines 394–431)**

```cpp
        RETCODE FindFirstOf(Predicate predicate, size_t& out_Record)
        {
            if (!m_IsOpen)
                return RTN_NULL_OBJ;

            bool found = false;
            size_t consecutive_skips = 0;
            size_t total_chunks = (m_NumRecords + m_ChunkRecords - 1) / m_ChunkRecords;
#ifndef WINDOWS_PLATFORM
            pthread_rwlock_t* dbLock = &reinterpret_cast<DBHeader*>(m_DBAddress)->m_DBLock;
#endif

            for (size_t chunk = 0; chunk < total_chunks && !found; chunk++)
            {
                // Attempt non-blocking read lock; fall back to blocking after READER_MAX_SKIP skips
                bool acquired = false;
                if (consecutive_skips < READER_MAX_SKIP)
                {
#ifdef WINDOWS_PLATFORM
                    // Windows has no trylock equivalent here; always block
                    if (RTN_OK != LockDB()) return RTN_LOCK_ERROR;
                    acquired = true;
                    consecutive_skips = 0;
#else
                    if (0 == pthread_rwlock_tryrdlock(dbLock))
                    {
                        m_WindowMutex.lock();
                        acquired = true;
                        consecutive_skips = 0;
                    }
                    else
                    {
                        ++consecutive_skips;
                        continue;
                    }
#endif
                }
                else
                {
                    if (RTN_OK != LockDB())
                        return RTN_LOCK_ERROR;
                    acquired = true;
                    consecutive_skips = 0;
                }

                size_t chunk_start = chunk * m_ChunkRecords;
                size_t chunk_end   = std::min(chunk_start + m_ChunkRecords, m_NumRecords);

                for (size_t record = chunk_start; record < chunk_end; record++)
                {
                    char* p = Get(record);
                    if (nullptr == p) break;
                    if (predicate(reinterpret_cast<const object*>(p)))
                    {
                        out_Record = record;
                        found = true;
                        break;
                    }
                }

                if (RTN_OK != UnlockDB())
                    return RTN_LOCK_ERROR;
            }

            return found ? RTN_OK : RTN_NOT_FOUND;
        }
```

- [ ] **Step 2: Build and test**

```bash
cd /home/kdunn/qcDB && make unitTest -j$(nproc) 2>&1 && /home/kdunn/qcDB/bin/unitTest
```

Expected: `ALL TESTS PASSED`

- [ ] **Step 3: Commit**

```bash
git -C /home/kdunn/qcDB add qcDB/qcDB.hh
git -C /home/kdunn/qcDB commit -m "feat: FindFirstOf() per-chunk lock with tryrdlock reader skip and anti-starvation"
```

---

## Task 8: `FindObjects()` and `FinderThread()` — Thread-Local Mmaps

**Files:**
- Modify: `qcDB/qcDB.hh`

Each `FindObjects` thread receives its chunk range and manages its own `mmap`/`munmap` per chunk. The main thread holds no DB lock; threads each use `tryrdlock`/blocking-rdlock per chunk independently.

- [ ] **Step 1: Replace `FinderThread()` static method (lines 752–762)**

```cpp
    static void FinderThread(Predicate predicate,
                             int fd,
                             pthread_rwlock_t* dbLock,
                             size_t startRecord,
                             size_t endRecord,
                             size_t chunkRecords,
                             size_t pageSize,
                             size_t fileSize,
                             std::vector<object>& results)
    {
#ifdef WINDOWS_PLATFORM
        // Windows windowed mmap is stubbed — thread produces no results.
        (void)fd; (void)dbLock; (void)startRecord; (void)endRecord;
        (void)chunkRecords; (void)pageSize; (void)fileSize;
        return;
#else
        size_t consecutive_skips = 0;
        size_t record = startRecord;

        while (record < endRecord)
        {
            size_t chunk_index = record / chunkRecords;
            size_t chunk_start = chunk_index * chunkRecords;
            size_t chunk_end   = std::min(chunk_start + chunkRecords, endRecord);

            // Acquire read lock — try non-blocking first, block after READER_MAX_SKIP skips
            if (consecutive_skips < READER_MAX_SKIP)
            {
                if (0 != pthread_rwlock_tryrdlock(dbLock))
                {
                    ++consecutive_skips;
                    record = chunk_end;
                    continue;
                }
                consecutive_skips = 0;
            }
            else
            {
                pthread_rwlock_rdlock(dbLock);
                consecutive_skips = 0;
            }

            // Map this chunk locally — independent of shared m_DataWindow
            size_t file_offset = sizeof(DBHeader) + chunk_start * sizeof(object);
            size_t aligned_off = (file_offset / pageSize) * pageSize;
            size_t extra       = file_offset - aligned_off;
            size_t data_bytes  = chunkRecords * sizeof(object);
            size_t map_bytes   = ((extra + data_bytes + pageSize - 1) / pageSize) * pageSize;
            if (aligned_off + map_bytes > fileSize)
                map_bytes = fileSize - aligned_off;

            char* window = static_cast<char*>(mmap(nullptr, map_bytes,
                PROT_READ | PROT_WRITE, MAP_SHARED, fd,
                static_cast<off_t>(aligned_off)));

            if (MAP_FAILED != window)
            {
                for (size_t r = record; r < chunk_end; r++)
                {
                    const object* obj = reinterpret_cast<const object*>(
                        window + extra + (r - chunk_start) * sizeof(object));
                    if (predicate(obj))
                        results.push_back(*obj);
                }
                munmap(window, map_bytes);
            }

            pthread_rwlock_unlock(dbLock);
            record = chunk_end;
        }
#endif
    }
```

- [ ] **Step 2: Replace `FindObjects()` (lines 437–491)**

```cpp
        RETCODE FindObjects(Predicate predicate, std::vector<object>& out_MatchingObjects)
        {
            if (!m_IsOpen)
                return RTN_NULL_OBJ;

#ifdef WINDOWS_PLATFORM
            size_t numThreads = std::thread::hardware_concurrency() / 2;
#else
            size_t numThreads = static_cast<size_t>(sysconf(_SC_NPROCESSORS_ONLN)) / 2;
#endif
            if (0 == numThreads) numThreads = 1;

            size_t total_chunks = (m_NumRecords + m_ChunkRecords - 1) / m_ChunkRecords;
            if (0 == total_chunks)
                return RTN_OK;  // empty DB
            numThreads = std::min(numThreads, total_chunks);
            if (0 == numThreads) numThreads = 1;

            std::vector<std::thread> threads;
            std::vector<std::vector<object>> results(numThreads);

            pthread_rwlock_t* dbLock =
                &reinterpret_cast<DBHeader*>(m_DBAddress)->m_DBLock;

            size_t chunksPerThread = total_chunks / numThreads;

            for (size_t threadIndex = 0; threadIndex < numThreads; threadIndex++)
            {
                size_t startChunk = threadIndex * chunksPerThread;
                size_t endChunk   = (threadIndex == numThreads - 1)
                                    ? total_chunks
                                    : startChunk + chunksPerThread;
                size_t startRecord = startChunk * m_ChunkRecords;
                size_t endRecord   = std::min(endChunk * m_ChunkRecords, m_NumRecords);

                threads.emplace_back(FinderThread, predicate, m_fd, dbLock,
                    startRecord, endRecord, m_ChunkRecords,
                    m_PageSize, m_Size, std::ref(results[threadIndex]));
            }

            for (std::thread& thread : threads)
                thread.join();

            for (std::vector<object>& matches : results)
                for (object& match : matches)
                    out_MatchingObjects.push_back(match);

            return RTN_OK;
        }
```

- [ ] **Step 3: Build and test**

```bash
cd /home/kdunn/qcDB && make unitTest -j$(nproc) 2>&1 && /home/kdunn/qcDB/bin/unitTest
```

Expected: `ALL TESTS PASSED`

- [ ] **Step 4: Commit**

```bash
git -C /home/kdunn/qcDB add qcDB/qcDB.hh
git -C /home/kdunn/qcDB commit -m "feat: FindObjects() and FinderThread() use thread-local mmaps with per-chunk reader skip"
```

---

## Task 9: Destructor Update

**Files:**
- Modify: `qcDB/qcDB.hh`

Destructor must unmap the data window, unmap the pinned header, and close `m_fd`.

- [ ] **Step 1: Replace the destructor (lines 634–647)**

```cpp
        ~dbInterface(void)
        {
#ifdef WINDOWS_PLATFORM
            if (nullptr != m_DataWindow)
                UnmapViewOfFile(m_DataWindow);
            if (nullptr != m_DBAddress)
                UnmapViewOfFile(m_DBAddress);
#else
            if (nullptr != m_DataWindow)
                munmap(m_DataWindow, m_WindowMappedBytes);
            if (nullptr != m_DBAddress)
                munmap(m_DBAddress, m_PageSize);
            if (INVALID_FD != m_fd)
                close(m_fd);
#endif
            m_IsOpen     = false;
            m_DataWindow = nullptr;
            m_DBAddress  = nullptr;
            m_fd         = INVALID_FD;
        }
```

- [ ] **Step 2: Build and test**

```bash
cd /home/kdunn/qcDB && make unitTest -j$(nproc) 2>&1 && /home/kdunn/qcDB/bin/unitTest
```

Expected: `ALL TESTS PASSED`

- [ ] **Step 3: Commit**

```bash
git -C /home/kdunn/qcDB add qcDB/qcDB.hh
git -C /home/kdunn/qcDB commit -m "feat: update destructor to unmap data window, header, and close fd"
```

---

## Task 10: dbGenerator Writer-Preference rwlock

**Files:**
- Modify: `dbGenerator/src/Schema.cpp`

Add `PTHREAD_RWLOCK_PREFER_WRITER_NONRECURSIVE_NP` to the rwlock attribute set during DB creation. This ensures that when a writer calls `wrlock()`, subsequent reader `rdlock()` calls block rather than proceeding ahead.

- [ ] **Step 1: Update rwlock init in `Schema.cpp` (lines 414–417)**

Find this block:
```cpp
    pthread_rwlockattr_t dbLockAttributest = {0};
    pthread_rwlockattr_init(&dbLockAttributest);
    pthread_rwlockattr_setpshared(&dbLockAttributest, PTHREAD_PROCESS_SHARED);
    pthread_rwlock_init(&dbHeader.m_DBLock, &dbLockAttributest);
    pthread_rwlockattr_destroy(&dbLockAttributest);
```

Replace with:
```cpp
    pthread_rwlockattr_t dbLockAttributest = {0};
    pthread_rwlockattr_init(&dbLockAttributest);
    pthread_rwlockattr_setpshared(&dbLockAttributest, PTHREAD_PROCESS_SHARED);
#ifndef WINDOWS_PLATFORM
    pthread_rwlockattr_setkind_np(&dbLockAttributest, PTHREAD_RWLOCK_PREFER_WRITER_NONRECURSIVE_NP);
#endif
    pthread_rwlock_init(&dbHeader.m_DBLock, &dbLockAttributest);
    pthread_rwlockattr_destroy(&dbLockAttributest);
```

- [ ] **Step 2: Build all targets**

```bash
cd /home/kdunn/qcDB && make -j$(nproc) 2>&1
```

Expected: all three targets (`dbGenerator`, `testDB`, `unitTest`) build without errors.

- [ ] **Step 3: Commit**

```bash
git -C /home/kdunn/qcDB add dbGenerator/src/Schema.cpp
git -C /home/kdunn/qcDB commit -m "feat: initialize rwlock with writer-preference attribute in dbGenerator"
```

---

## Task 11: Integration Test with Existing testDB

The existing `testDB` binary forks multiple reader and writer processes over a real `CHARACTER` database and verifies throughput. Running it confirms the windowed mmap works correctly under real multi-process concurrent load.

- [ ] **Step 1: Regenerate the CHARACTER database with updated dbGenerator (for writer-preference rwlock)**

```bash
/home/kdunn/qcDB/bin/dbGenerator -s /home/kdunn/qcDB/schemaFiles/character.skm -d /tmp/ -h /tmp/
```

Expected: generates `/tmp/CHARACTER.db` and `/tmp/CHARACTER.hh`.

- [ ] **Step 2: Run unit tests one final time against clean state**

```bash
/home/kdunn/qcDB/bin/unitTest
```

Expected: `ALL TESTS PASSED`

- [ ] **Step 3: Run integration test (1 second, 4 reader+writer process pairs)**

```bash
/home/kdunn/qcDB/bin/testDB -d /tmp/CHARACTER.db -s 1 -p 4
```

Expected: prints total reads and writes, reports found matching records, exits 0. No error output.

- [ ] **Step 4: Final commit**

```bash
git -C /home/kdunn/qcDB add -A
git -C /home/kdunn/qcDB commit -m "feat: complete chunked windowed mmap implementation with writer-priority locking"
```

---

## Implementation Notes

- **Windows windowed path**: `SlideWindow` on Windows stubs out with `m_IsOpen = false` because the windowed path requires storing the file `HANDLE` across calls (Windows closes the HANDLE in the original constructor). Full Windows support requires adding a `HANDLE m_hFile` member alongside `m_fd` and implementing `MapViewOfFile` with the stored handle. The existing Windows full-mmap behavior is replaced by the stub until that work is done.

- **Existing db files**: The writer-preference rwlock attribute is a one-time initialization done by `dbGenerator`. Existing `.db` files created with the old `dbGenerator` will have a standard rwlock. Regenerate those files with the new `dbGenerator` to get writer-preference behavior.

- **`ReadObjects`/`WriteObjects(vector<tuple>)` chunk grouping**: These ops already sort records before locking, then call `Get()` per record inside the lock. Because `Get()` caches the current chunk and only calls `SlideWindow()` on a miss, sorted access naturally groups records by chunk — consecutive records in the same chunk share the cached window with zero mmap overhead. No code change is needed in these methods beyond the lock/`Get()` order fix applied in Task 5.

- **`READER_MAX_SKIP` tuning**: The constant (5) controls how many consecutive chunk skips readers tolerate before forcing a blocking lock. Higher values give writers more leeway at the cost of readers potentially missing more data per scan cycle. Change the constant in the member section of `qcDB.hh`.
