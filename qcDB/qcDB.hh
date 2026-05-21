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
            RETCODE retcode = RTN_OK;
            char* p_object = Get(record);
            if(nullptr == p_object)
            {
                return RTN_NULL_OBJ;
            }

            retcode = LockDB();
            if (RTN_OK != retcode)
            {
                return retcode;
            }

            memcpy(&out_object, p_object, sizeof(object));

            retcode = UnlockDB();
            if (RTN_OK != retcode)
            {
                return retcode;
            }

            return RTN_OK;
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
            RETCODE retcode = RTN_OK;
            char* p_object = Get(record);
            if(nullptr == p_object)
            {
                return RTN_NULL_OBJ;
            }

            retcode = WriteLockDB();
            if (RTN_OK != retcode)
            {
                return retcode;
            }

            DBHeader* header = reinterpret_cast<DBHeader*>(m_DBAddress);
            header->m_LastWritten = record;
            if (header->m_Size < record)
            {
                header->m_Size = record;
            }

            memcpy(p_object, &objectWrite, sizeof(object));

            retcode = UnlockDB();
            if (RTN_OK != retcode)
            {
                return retcode;
            }

            return RTN_OK;
        }

        /*
         * Write an object at the next available (empty) record.
         */
        RETCODE WriteObject(object& objectWrite)
        {
            RETCODE retcode = RTN_OK;
            object emptyObject = { 0 };
            retcode = WriteLockDB();
            if (RTN_OK != retcode)
            {
                return retcode;
            }

            DBHeader* header = reinterpret_cast<DBHeader*>(m_DBAddress);
            size_t record = header->m_LastWritten;
            object* currentObject = reinterpret_cast<object*>(m_DBAddress + sizeof(DBHeader) + record * sizeof(object));
            for (; record < NumberOfRecords(); record++)
            {
                if (memcmp(currentObject, &emptyObject, sizeof(object)) == 0)
                {
                    header->m_LastWritten = record;
                    memcpy(currentObject, &objectWrite, sizeof(object));

                    if (header->m_Size < record)
                    {
                        header->m_Size = record;
                    }

                    break;
                }

                currentObject++;
            }

            retcode = UnlockDB();
            if (RTN_OK != retcode)
            {
                return retcode;
            }

            if(NumberOfRecords() == record)
            {
                return RTN_NOT_FOUND;
            }

            return RTN_OK;
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
            RETCODE retcode = RTN_OK;
            const object emptyObject = { 0 };
            auto objectsIterator = objects.begin();

            retcode = WriteLockDB();
            if (RTN_OK != retcode)
            {
                return retcode;
            }

            DBHeader* header = reinterpret_cast<DBHeader*>(m_DBAddress);
            size_t record = header->m_LastWritten;
            object* currentObject = reinterpret_cast<object*>(m_DBAddress + sizeof(DBHeader) + record * sizeof(object));
            for (; record < NumberOfRecords(); record++)
            {
                if (0 == memcmp(currentObject, &emptyObject, sizeof(object)))
                {
                    if (objectsIterator == objects.end())
                    {
                        header->m_LastWritten = record;
                        break;
                    }

                    memcpy(currentObject, &(*objectsIterator), sizeof(object));

                    ++objectsIterator;
                }

                currentObject++;
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

            if (objectsIterator != objects.end())
            {
                return RTN_EOF;
            }

            return RTN_OK;
        }

        /*
         * Clear out the data at a given record.
         */
        RETCODE DeleteObject(size_t record)
        {
            RETCODE retcode = RTN_OK;
            object deletedObject = { 0 };
            char* p_object = Get(record);
            if (nullptr == p_object)
            {
                return RTN_NULL_OBJ;
            }

            retcode = WriteLockDB();
            if (RTN_OK != retcode)
            {
                return retcode;
            }
            DBHeader* header = reinterpret_cast<DBHeader*>(m_DBAddress);
            size_t size = header->m_Size;

            memset(p_object, 0, sizeof(object));

            // If the deleted record was the last one, find the next last record
            if (size == record)
            {
                for (p_object -= sizeof(object);
                     p_object >= m_DBAddress + sizeof(DBHeader);
                     p_object -= sizeof(object))
                {
                    --record;
                    if (memcmp(p_object, &deletedObject, sizeof(object)) != 0)
                    {
                        break;
                    }
                }

                if (p_object < m_DBAddress + sizeof(DBHeader))
                {
                    record = 0;
                }

                header->m_Size = record;
            }

            retcode = UnlockDB();
            if (RTN_OK != retcode)
            {
                return retcode;
            }

            return RTN_OK;
        }

        /*
         * Zero out all data from the database.
         */
        RETCODE Clear(void)
        {
            RETCODE retcode = RTN_OK;
            if (m_IsOpen)
            {
                retcode = WriteLockDB();
                if (RTN_OK != retcode)
                {
                    return retcode;
                }

                DBHeader* header = reinterpret_cast<DBHeader*>(m_DBAddress);
                size_t dbSize = header->m_NumRecords * sizeof(object);
                object* start = reinterpret_cast<object*>(m_DBAddress + sizeof(DBHeader));

                memset(start, 0, dbSize);

                header->m_LastWritten = 0;
                header->m_Size = 0;

                retcode = UnlockDB();
                if (RTN_OK != retcode)
                {
                    return retcode;
                }

                return RTN_OK;
            }

            return RTN_MALLOC_FAIL;
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
            RETCODE retcode = RTN_OK;
            bool found = false;
            retcode = LockDB();
            if (RTN_OK != retcode)
            {
                return retcode;
            }

            const object* currentObject = reinterpret_cast<const object*>(m_DBAddress + sizeof(DBHeader));
            size_t size = reinterpret_cast<DBHeader*>(m_DBAddress)->m_Size;
            size_t record = 0;
            for (record = 0; record <= size; record++)
            {
                if (predicate(currentObject))
                {
                    out_Record = record;
                    found = true;
                    break;
                }

                currentObject++;
            }

            retcode = UnlockDB();
            if (RTN_OK != retcode)
            {
                return retcode;
            }

            if(!found)
            {
                return RTN_NOT_FOUND;
            }

            return RTN_OK;
        }

        /*
         * Find objects using the predicate by sharding the database and searching
         * in parallel.
         */
        RETCODE FindObjects(Predicate predicate, std::vector<object>& out_MatchingObjects)
        {
            RETCODE retcode = RTN_OK;
            // Number of threads /2 so we don't completely lock up the CPU
#ifdef WINDOWS_PLATFORM
            size_t numThreads = std::thread::hardware_concurrency() / 2;
#else
            size_t numThreads = sysconf(_SC_NPROCESSORS_ONLN) / 2;
#endif
            if (0 == numThreads)
            {
                numThreads = 1;
            }
            std::vector<std::thread> threads;
            std::vector<std::vector<object>> results(numThreads);

            retcode = LockDB();
            if (RTN_OK != retcode)
            {
                return retcode;
            }

            const object* currentObject = reinterpret_cast<const object*>(m_DBAddress + sizeof(DBHeader));
            size_t size = reinterpret_cast<DBHeader*>(m_DBAddress)->m_Size;
            size_t segmentSize = size / numThreads;

            for (size_t threadIndex = 0; threadIndex < numThreads - 1; threadIndex++)
            {
                threads.emplace_back(FinderThread, predicate, currentObject, segmentSize, std::ref(results[threadIndex]));
                currentObject += segmentSize;
            }

            threads.emplace_back(FinderThread, predicate, currentObject, size + 1 - ((numThreads - 1) * segmentSize), std::ref(results[numThreads - 1]));

            for (std::thread& thread : threads)
            {
                thread.join();
            }

            retcode = UnlockDB();
            if (RTN_OK != retcode)
            {
                return retcode;
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

        ~dbInterface(void)
        {
#ifdef WINDOWS_PLATFORM
            // Unmap the file view
            UnmapViewOfFile(m_DBAddress);

    #else
            int error = munmap(m_DBAddress, m_Size);
            if(0 == error)
            {
                m_IsOpen = false;
            }
#endif
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

        return RTN_OK;
    }

    /*
     * Unlock the DB. Several ways to do this depending on OS.
     */
    RETCODE UnlockDB(void)
    {
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

        return RTN_OK;
    }

    /*
     * Get a pointer into the database according to the record number.
     * Returns a nullptr on error.
     */
    char* Get(const size_t record)
    {
        if(NumberOfRecords() - 1 < record)
        {
            return nullptr;
        }

        if(!m_IsOpen || nullptr == m_DBAddress)
        {
            return nullptr;
        }

        // Record off by 1 adjustment
        size_t byte_index = sizeof(DBHeader) + sizeof(object) * record;
        if(m_Size < byte_index)
        {
            return nullptr;
        }

        return m_DBAddress + byte_index;
    }

    /*
     * Internal thread function that is used to run the predicate
     * in parallel in the sharded database.
     */
    static void FinderThread(Predicate predicate, const object* currentObject, size_t numRecords, std::vector<object>& results)
    {
        for (size_t record = 0; record < numRecords; record++)
        {
            if (predicate(currentObject))
            {
                results.push_back(*currentObject);
            }
            currentObject++;
        }
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

    std::mutex m_WindowMutex;    // serializes window slides within a process

#ifdef WINDOWS_PLATFORM
    HANDLE m_Mutex;
#endif

    static constexpr int    INVALID_FD       = -1;
    static constexpr size_t READER_MAX_SKIP  = 5;

    };
}

#endif