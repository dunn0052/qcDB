#ifndef __DB_HEADER_HH
#define __DB_HEADER_HH

#include <common/OSdefines.hh>
#include <cstdint>

#ifdef WINDOWS_PLATFORM
using pthread_rwlock_t = char[80];
#else
#include <pthread.h>
#endif

constexpr size_t MAX_SCHEMA_FIELDS = 64;

constexpr uint8_t FIELD_FLAG_READONLY = 0x01;
constexpr uint8_t FIELD_FLAG_METADATA = 0x02;

/*
 * Fixed 40-byte descriptor for one schema field.
 * Layout is explicit — no compiler-inserted padding.
 * Stored in the file immediately after DBHeader.
 */
struct FieldDescriptor
{
    char     m_Name[32];       // field name, null-terminated, max 31 chars
    char     m_Type;           // i I l L ? c b x
    uint8_t  m_Flags;          // FIELD_FLAG_READONLY | FIELD_FLAG_METADATA
    uint8_t  m_SchemaVersion;  // schema version that introduced this field (0 = original)
    uint8_t  _pad;             // reserved, must be zero
    uint16_t m_Count;          // array length (1 for scalar)
    uint16_t m_FieldOffset;    // byte offset of field within the record struct
};
static_assert(sizeof(FieldDescriptor) == 40,        "FieldDescriptor must be 40 bytes");
static_assert(offsetof(FieldDescriptor, m_Name)          ==  0, "m_Name offset wrong");
static_assert(offsetof(FieldDescriptor, m_Type)          == 32, "m_Type offset wrong");
static_assert(offsetof(FieldDescriptor, m_Flags)         == 33, "m_Flags offset wrong");
static_assert(offsetof(FieldDescriptor, m_SchemaVersion) == 34, "m_SchemaVersion offset wrong");
static_assert(offsetof(FieldDescriptor, _pad)            == 35, "_pad offset wrong");
static_assert(offsetof(FieldDescriptor, m_Count)         == 36, "m_Count offset wrong");
static_assert(offsetof(FieldDescriptor, m_FieldOffset)   == 38, "m_FieldOffset offset wrong");

/*
 * DBHeader values must be accessed before fields to remain cache friendly.
 * FieldDescriptor[] array follows immediately after this struct in the file.
 * Record base offset = sizeof(DBHeader) + m_SchemaSize
 */
struct DBHeader
{
    pthread_rwlock_t m_DBLock;
    char     m_ObjectName[24];
    size_t   m_NumRecords;
    size_t   m_LastWritten;
    size_t   m_Size;           // max record index written
    uint32_t m_SchemaSize;     // FieldDescriptor[] byte count = m_NumFields * 40
    uint32_t m_RecordSize;     // sizeof(record struct) — validated on open
    uint8_t  m_NumFields;      // number of FieldDescriptors following this header
    uint8_t  _pad[7];          // explicit padding, must be zero
};

#endif
