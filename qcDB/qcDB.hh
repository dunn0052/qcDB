#ifndef __QC_DB_HH
#define __QC_DB_HH

#include <common/OSdefines.hh>

#include <string>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#ifdef WINDOWS_PLATFORM
#include <Windows.h>
#else
#include <sys/mman.h>
#endif
#include <iostream>
#include <algorithm>
#include <cstring>
#include <functional>
#include <mutex>
#include <climits>
#include <thread>
#include <vector>

#ifndef WINDOWS_PLATFORM
#include <time.h>
#endif

#include <common/Retcode.hh>
#include <common/DBHeader.hh>

namespace qcDB
{
    template <class object>
    class dbInterface
    {

public:

        /*
         * Read object at given record.
         */
        RETCODE ReadObject(size_t record, object& out_object)
        {
            if (!m_IsOpen || record >= m_NumRecords)
            {
                return RTN_NULL_OBJ;
            }

            RETCODE retcode = LockDB();
            if (RTN_OK != retcode)
            {
                return retcode;
            }

            char* p_object = Get(record);
            if (nullptr == p_object)
            {
                UnlockDB();
                return RTN_NULL_OBJ;
            }

            memcpy(&out_object, p_object, sizeof(object));
            return UnlockDB();
        }

        /*
         * Read several objects given a vector of tuples <record, empty object>.
         * The empty object will have the data of the object of the given record
         * copied into it.
         */
        RETCODE ReadObjects(std::vector<std::tuple<size_t, object>>& objects)
        {
            RETCODE retcode = RTN_OK;
            // Sort here to be cache friendly
            std::sort(objects.begin(), objects.end(),
                [](const std::tuple<size_t, object>& a, const std::tuple<size_t, object>& b) {
                return std::get<0>(a) < std::get<0>(b);
                });

            retcode = LockDB();
            if (RTN_OK != retcode)
            {
                return retcode;
            }


            for(std::tuple<size_t, object>& readObject : objects)
            {
                char* p_object = Get(std::get<0>(readObject));
                if(nullptr == p_object)
                {
                    UnlockDB();
                    return RTN_NULL_OBJ;
                }
                memcpy(&std::get<1>(readObject), p_object, sizeof(object));
            }

            retcode = UnlockDB();
            if (RTN_OK != retcode)
            {
                return retcode;
            }

            return RTN_OK;
        }

        /*
         * Overwrite object data at given record.
         */
        RETCODE WriteObject(size_t record, object& objectWrite)
        {
            if (!m_IsOpen || record >= m_NumRecords)
            {
                return RTN_NULL_OBJ;
            }

            RETCODE retcode = WriteLockDB();
            if (RTN_OK != retcode)
            {
                return retcode;
            }

            char* p_object = Get(record);
            if (nullptr == p_object)
            {
                UnlockDB();
                return RTN_NULL_OBJ;
            }

            DBHeader* header = reinterpret_cast<DBHeader*>(m_DBAddress);
            header->m_LastWritten = record;
            if (header->m_Size < record)
            {
                header->m_Size = record;
            }

            memcpy(p_object, &objectWrite, sizeof(object));
            *reinterpret_cast<unsigned long*>(p_object)                         = static_cast<unsigned long>(record);
            *reinterpret_cast<unsigned long*>(p_object + sizeof(unsigned long))  = MetadataTimestamp();
            return UnlockDB();
        }

        /*
         * Write an object at the next available (empty) record.
         */
        RETCODE WriteObject(object& objectWrite)
        {
            const object emptyObject = {};
            RETCODE retcode = WriteLockDB();
            if (RTN_OK != retcode)
            {
                return retcode;
            }

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
                    *reinterpret_cast<unsigned long*>(p)                        = static_cast<unsigned long>(record);
                    *reinterpret_cast<unsigned long*>(p + sizeof(unsigned long)) = MetadataTimestamp();
                    if (header->m_Size < record)
                    {
                        header->m_Size = record;
                    }
                    found = true;
                    break;
                }
            }

            retcode = UnlockDB();
            if (RTN_OK != retcode)
            {
                return retcode;
            }

            return found ? RTN_OK : RTN_NOT_FOUND;
        }

        /*
         * Write multiple objects by record. The vector is a list of <record, object data> to
         * be overwritten at the corresponding record.
         */
        RETCODE WriteObjects(std::vector<std::tuple<size_t, object>>& objects)
        {

            RETCODE retcode = RTN_OK;
            std::sort(objects.begin(), objects.end(),
                [](const std::tuple<size_t, object>& first, const std::tuple<size_t, object>& second)
                {
                    return std::get<0>(first) < std::get<0>(second);
                }
            );

            retcode = WriteLockDB();
            if (RTN_OK != retcode)
            {
                return retcode;
            }

            for(std::tuple<size_t, object>& writeObject : objects)
            {
                char* p_object = Get(std::get<0>(writeObject));
                if(nullptr == p_object)
                {
                    UnlockDB();
                    return RTN_NULL_OBJ;
                }
                memcpy(p_object, &std::get<1>(writeObject), sizeof(object));
                *reinterpret_cast<unsigned long*>(p_object)                         = static_cast<unsigned long>(std::get<0>(writeObject));
                *reinterpret_cast<unsigned long*>(p_object + sizeof(unsigned long))  = MetadataTimestamp();
            }

            DBHeader* header = reinterpret_cast<DBHeader*>(m_DBAddress);
            header->m_LastWritten = std::get<0>(objects.back());

            if (header->m_Size < std::get<0>(objects.back()))
            {
                header->m_Size = std::get<0>(objects.back());
            }

            retcode = UnlockDB();
            if (RTN_OK != retcode)
            {
                return retcode;
            }

            return RTN_OK;
        }

        /*
         * Write multiple objects at the next available (empty) records.
         */
        RETCODE WriteObjects(std::vector<object>& objects)
        {
            const object emptyObject = {};
            auto objectsIterator = objects.begin();

            RETCODE retcode = WriteLockDB();
            if (RTN_OK != retcode)
            {
                return retcode;
            }

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
                    *reinterpret_cast<unsigned long*>(p)                        = static_cast<unsigned long>(record);
                    *reinterpret_cast<unsigned long*>(p + sizeof(unsigned long)) = MetadataTimestamp();
                    ++objectsIterator;
                }
            }

            if (header->m_Size < record)
            {
                header->m_Size = record;
            }

            retcode = UnlockDB();
            if (RTN_OK != retcode)
            {
                return retcode;
            }

            return (objectsIterator != objects.end()) ? RTN_EOF : RTN_OK;
        }

        /*
         * Clear out the data at a given record.
         */
        RETCODE DeleteObject(size_t record)
        {
            if (!m_IsOpen || record >= m_NumRecords)
            {
                return RTN_NULL_OBJ;
            }

            RETCODE retcode = WriteLockDB();
            if (RTN_OK != retcode)
            {
                return retcode;
            }

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
                size_t newSize = 0;
                for (size_t i = record; i-- > 0;)
                {
                    char* p = Get(i);
                    if (nullptr == p)
                    {
                        break;
                    }
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

        /*
         * Zero out all data from the database.
         */
        RETCODE Clear(void)
        {
            if (!m_IsOpen)
            {
                return RTN_MALLOC_FAIL;
            }

            RETCODE retcode = WriteLockDB();
            if (RTN_OK != retcode)
            {
                return retcode;
            }

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

        /*
         * A user defined search function.
         * Example lambda function
         * [](const CHARACTER* character) -> bool
         * {
         *    return !strcmp(character->NAME, "KEVIN");
         * }
         */
        using Predicate = std::function<bool(const object* currentObject)>;

        /*
         * If multiple records would match the predicate,
         * return the record of the first one found.
         */
        RETCODE FindFirstOf(Predicate predicate, size_t& out_Record)
        {
            if (!m_IsOpen)
            {
                return RTN_NULL_OBJ;
            }

            bool found = false;
            size_t consecutive_skips = 0;
            size_t total_chunks = (m_NumRecords + m_ChunkRecords - 1) / m_ChunkRecords;
#ifndef WINDOWS_PLATFORM
            pthread_rwlock_t* dbLock = &reinterpret_cast<DBHeader*>(m_DBAddress)->m_DBLock;
#endif

            for (size_t chunk = 0; chunk < total_chunks && !found; chunk++)
            {
                if (consecutive_skips < READER_MAX_SKIP)
                {
#ifdef WINDOWS_PLATFORM
                    if (RTN_OK != LockDB())
                    {
                        return RTN_LOCK_ERROR;
                    }
                    consecutive_skips = 0;
#else
                    if (0 == pthread_rwlock_tryrdlock(dbLock))
                    {
                        m_WindowMutex.lock();
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
                    {
                        return RTN_LOCK_ERROR;
                    }
                    consecutive_skips = 0;
                }

                size_t chunk_start = chunk * m_ChunkRecords;
                size_t chunk_end   = std::min(chunk_start + m_ChunkRecords, m_NumRecords);

                for (size_t record = chunk_start; record < chunk_end; record++)
                {
                    char* p = Get(record);
                    if (nullptr == p)
                    {
                        break;
                    }
                    if (predicate(reinterpret_cast<const object*>(p)))
                    {
                        out_Record = record;
                        found = true;
                        break;
                    }
                }

                if (RTN_OK != UnlockDB())
                {
                    return RTN_LOCK_ERROR;
                }
            }

            return found ? RTN_OK : RTN_NOT_FOUND;
        }

        /*
         * Find objects using the predicate by sharding the database and searching
         * in parallel.
         */
        RETCODE FindObjects(Predicate predicate, std::vector<object>& out_MatchingObjects)
        {
            if (!m_IsOpen)
            {
                return RTN_NULL_OBJ;
            }

#ifdef WINDOWS_PLATFORM
            size_t numThreads = std::thread::hardware_concurrency() / 2;
#else
            size_t numThreads = static_cast<size_t>(sysconf(_SC_NPROCESSORS_ONLN)) / 2;
#endif
            if (0 == numThreads)
            {
                numThreads = 1;
            }

            size_t total_chunks = (m_NumRecords + m_ChunkRecords - 1) / m_ChunkRecords;
            if (0 == total_chunks)
            {
                return RTN_OK;
            }
            numThreads = std::min(numThreads, total_chunks);
            if (0 == numThreads)
            {
                numThreads = 1;
            }

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
            {
                thread.join();
            }

            for (std::vector<object>& matches : results)
            {
                for (object& match : matches)
                {
                    out_MatchingObjects.push_back(match);
                }
            }

            return RTN_OK;
        }

        /*
         * Total number of records to be accessed by users.
         */
        inline size_t NumberOfRecords(void)
        {
            if (m_IsOpen)
            {
                return m_NumRecords;
            }

            return 0;
        }

        /*
         * Gather last written record for users.
         */
        inline size_t LastWrittenRecord(size_t& record)
        {
            RETCODE retcode = RTN_OK;
            size_t lastWrittenRecord = 0;

            if (m_IsOpen)
            {
                retcode = LockDB();
                if (RTN_OK != retcode)
                {
                    return static_cast<size_t>(-1);
                }

                record = reinterpret_cast<DBHeader*>(m_DBAddress)->m_LastWritten;

                retcode = UnlockDB();
                if (RTN_OK != retcode)
                {
                    return static_cast<size_t>(-1);
                }

            }
            else
            {
                return RTN_NULL_OBJ;
            }

            return retcode;
        }

        dbInterface(const std::string& dbPath) :
            m_IsOpen(false), m_Size(0),
            m_DBAddress(nullptr), m_NumRecords(0),
            m_fd(INVALID_FD), m_DataWindow(nullptr),
            m_WindowChunkIndex(SIZE_MAX), m_WindowMappedBytes(0),
            m_WindowExtra(0), m_ChunkRecords(1), m_PageSize(4096),
            m_SchemaOffset(0)
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
            {
                return;
            }

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
            {
                return;
            }

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
            DBHeader* hdr = reinterpret_cast<DBHeader*>(m_DBAddress);
            m_NumRecords = hdr->m_NumRecords;

            // Validate record size matches this template instantiation
            if (hdr->m_RecordSize != static_cast<uint32_t>(sizeof(object)))
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

            // Validate schema block invariant
            if (hdr->m_SchemaSize !=
                static_cast<uint32_t>(hdr->m_NumFields * sizeof(FieldDescriptor)))
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

            m_SchemaOffset = hdr->m_SchemaSize;
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

        ~dbInterface(void)
        {
#ifdef WINDOWS_PLATFORM
            if (nullptr != m_DataWindow)
            {
                UnmapViewOfFile(m_DataWindow);
            }
            if (nullptr != m_DBAddress)
            {
                UnmapViewOfFile(m_DBAddress);
            }
#else
            if (nullptr != m_DataWindow)
            {
                munmap(m_DataWindow, m_WindowMappedBytes);
            }
            if (nullptr != m_DBAddress)
            {
                munmap(m_DBAddress, m_PageSize);
            }
            if (INVALID_FD != m_fd)
            {
                close(m_fd);
            }
#endif
            m_IsOpen     = false;
            m_DataWindow = nullptr;
            m_DBAddress  = nullptr;
            m_fd         = INVALID_FD;
        }

protected:

    /*
     * Lock the DB. Several ways to do this depending on OS.
     */
    RETCODE LockDB(void)
    {
#ifdef WINDOWS_PLATFORM
        m_Mutex = CreateMutexA(NULL, FALSE, "MutexForFileLock");
        if (nullptr == m_Mutex)
        {
            return RTN_LOCK_ERROR;
        }
        if (WAIT_FAILED == WaitForSingleObject(m_Mutex, INFINITE))
        {
            return RTN_LOCK_ERROR;
        }
#else
        int lockError = pthread_rwlock_rdlock(&reinterpret_cast<DBHeader*>(m_DBAddress)->m_DBLock);
        if (0 != lockError)
        {
            return RTN_LOCK_ERROR;
        }
#endif
        m_WindowMutex.lock();
        return RTN_OK;
    }

    /*
     * Unlock the DB. Several ways to do this depending on OS.
     */
    RETCODE UnlockDB(void)
    {
        m_WindowMutex.unlock();
#ifdef WINDOWS_PLATFORM
        if (!ReleaseMutex(m_Mutex))
        {
            return RTN_LOCK_ERROR;
        }
#else
        int lockError = pthread_rwlock_unlock(&reinterpret_cast<DBHeader*>(m_DBAddress)->m_DBLock);
        if (0 != lockError)
        {
            return RTN_LOCK_ERROR;
        }
#endif
        return RTN_OK;
    }

    RETCODE WriteLockDB(void)
    {
#ifdef WINDOWS_PLATFORM
        m_Mutex = CreateMutexA(NULL, FALSE, "MutexForFileLock");
        if (nullptr == m_Mutex)
        {
            return RTN_LOCK_ERROR;
        }
        if (WAIT_FAILED == WaitForSingleObject(m_Mutex, INFINITE))
        {
            return RTN_LOCK_ERROR;
        }
#else
        int lockError = pthread_rwlock_wrlock(&reinterpret_cast<DBHeader*>(m_DBAddress)->m_DBLock);
        if (0 != lockError)
        {
            return RTN_LOCK_ERROR;
        }
#endif
        m_WindowMutex.lock();
        return RTN_OK;
    }

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
        size_t file_offset  = sizeof(DBHeader) + m_SchemaOffset + chunk_start * sizeof(object);
        size_t aligned_off  = (file_offset / m_PageSize) * m_PageSize;
        size_t extra        = file_offset - aligned_off;
        size_t data_bytes   = m_ChunkRecords * sizeof(object);
        size_t map_bytes    = ((extra + data_bytes + m_PageSize - 1) / m_PageSize) * m_PageSize;

        if (aligned_off + map_bytes > m_Size)
        {
            map_bytes = m_Size - aligned_off;
        }

        char* window = nullptr;
#ifdef WINDOWS_PLATFORM
        // Full Windows windowed implementation requires storing the file HANDLE.
        // Stub: mark closed so callers fall back gracefully.
        m_IsOpen = false;
        return false;
#else
        window = static_cast<char*>(mmap(nullptr, map_bytes,
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

    /*
     * Get a pointer into the database according to the record number.
     * Returns a nullptr on error.
     */
    char* Get(const size_t record)
    {
        if (!m_IsOpen || nullptr == m_DBAddress || record >= m_NumRecords)
        {
            return nullptr;
        }

        size_t chunk_index = record / m_ChunkRecords;
        if (chunk_index != m_WindowChunkIndex || nullptr == m_DataWindow)
        {
            if (!SlideWindow(record))
            {
                return nullptr;
            }
        }

        size_t chunk_start = m_WindowChunkIndex * m_ChunkRecords;
        return m_DataWindow + m_WindowExtra + (record - chunk_start) * sizeof(object);
    }

    /*
     * Internal thread function that is used to run the predicate
     * in parallel in the sharded database.
     */
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

            size_t file_offset = sizeof(DBHeader) + chunk_start * sizeof(object);
            size_t aligned_off = (file_offset / pageSize) * pageSize;
            size_t extra       = file_offset - aligned_off;
            size_t data_bytes  = chunkRecords * sizeof(object);
            size_t map_bytes   = ((extra + data_bytes + pageSize - 1) / pageSize) * pageSize;
            if (aligned_off + map_bytes > fileSize)
            {
                map_bytes = fileSize - aligned_off;
            }

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
                    {
                        results.push_back(*obj);
                    }
                }
                munmap(window, map_bytes);
            }

            pthread_rwlock_unlock(dbLock);
            record = chunk_end;
        }
#endif
    }

    static unsigned long MetadataTimestamp()
    {
#ifdef WINDOWS_PLATFORM
        FILETIME ft;
        GetSystemTimeAsFileTime(&ft);
        ULARGE_INTEGER uli;
        uli.LowPart  = ft.dwLowDateTime;
        uli.HighPart = ft.dwHighDateTime;
        return static_cast<unsigned long>(uli.QuadPart * 100ULL);
#else
        struct timespec ts;
        clock_gettime(CLOCK_REALTIME, &ts);
        return static_cast<unsigned long>(ts.tv_sec) * 1000000000UL
             + static_cast<unsigned long>(ts.tv_nsec);
#endif
    }

    bool   m_IsOpen;
    size_t m_Size;
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
    size_t m_SchemaOffset;       // bytes of schema block after DBHeader (= m_SchemaSize)

    std::mutex m_WindowMutex;    // serializes window slides within a process

#ifdef WINDOWS_PLATFORM
    HANDLE m_Mutex;
#endif

    static constexpr int    INVALID_FD       = -1;
    static constexpr size_t READER_MAX_SKIP  = 5;

    };
}

#endif
