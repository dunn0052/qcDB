#ifndef __QC_DB_HH
#define __QC_DB_HH

#include <common/OSdefines.hh>

#include <string>
#include <fcntl.h>
#include <sys/stat.h>

#ifdef WINDOWS_PLATFORM
#include <Windows.h>
#else
#include <sys/mman.h>
#endif

#include <iostream>

#include <common/Retcode.hh>
#include <common/DBHeader.hh>

namespace qcDB
{
    template <class object>
    class dbInterface
    {

public:

        RETCODE ReadObject(size_t record, object& out_object)
        {
            char* p_object = Get(record);
            if(nullptr == p_object)
            {
                return RTN_NULL_OBJ;
            }

            int lockError = pthread_rwlock_rdlock(&reinterpret_cast<DBHeader*>(m_DBAddress)->m_DBLock);
            if(0 != lockError)
            {
                return RTN_LOCK_ERROR;
            }

            memcpy(&out_object, p_object, sizeof(object));

            lockError = pthread_rwlock_unlock(&reinterpret_cast<DBHeader*>(m_DBAddress)->m_DBLock);
            if(0 != lockError)
            {
                return RTN_LOCK_ERROR;
            }

            return RTN_OK;
        }

        RETCODE WriteObject(size_t record, object& objectWrite)
        {
            char* p_object = Get(record);
            if(nullptr == p_object)
            {
                return RTN_NULL_OBJ;
            }

            int lockError = pthread_rwlock_wrlock(&reinterpret_cast<DBHeader*>(m_DBAddress)->m_DBLock);
            if(0 != lockError)
            {
                return RTN_LOCK_ERROR;
            }

            memcpy(p_object, &objectWrite, sizeof(object));

            lockError = pthread_rwlock_unlock(&reinterpret_cast<DBHeader*>(m_DBAddress)->m_DBLock);
            if(0 != lockError)
            {
                return RTN_LOCK_ERROR;
            }

            return RTN_OK;
        }

        inline size_t NumberOfRecords(void)
        {
            if(m_IsOpen)
            {
                return m_NumRecords;
            }

            return 0;
        }

        dbInterface(const std::string& dbPath) :
            m_IsOpen(false), m_Size(0),
            m_DBAddress(nullptr)
        {
            int fd = open(dbPath.c_str(), O_RDWR);
            if(INVALID_FD > fd)
            {
                return;
            }

            struct stat statbuf;
            int error = fstat(fd, &statbuf);
            if(0 > error)
            {
                return;
            }

            m_Size = statbuf.st_size;
            m_DBAddress = static_cast<char*>(mmap(nullptr, m_Size,
                    PROT_READ | PROT_WRITE, MAP_SHARED | MAP_POPULATE,
                    fd, 0));
            if(MAP_FAILED == m_DBAddress)
            {
                return;
            }

            m_NumRecords = reinterpret_cast<DBHeader*>(m_DBAddress)->m_NumRecords;

            m_IsOpen = true;
        }

        ~dbInterface(void)
        {
            int error = munmap(m_DBAddress, m_Size);
            if(0 == error)
            {
                // Nothing much you can do in this case..
                m_IsOpen = false;
            }
        }

protected:

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

    bool m_IsOpen;
    size_t m_Size;
    size_t m_NumRecords;
    char* m_DBAddress;

    static constexpr int INVALID_FD = 0;

    };
}

#endif