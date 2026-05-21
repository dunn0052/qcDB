#include <common/DBHeader.hh>

#include <cstdio>
#include <cstring>
#include <cctype>
#include <cerrno>
#include <climits>
#include <cstdlib>
#include <string>

#include <fcntl.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <pthread.h>
#include <time.h>

static size_t TypeSize(char type)
{
    switch (type)
    {
        case 'i': return sizeof(int);
        case 'I': return sizeof(unsigned int);
        case 'l': return sizeof(long);
        case 'L': return sizeof(unsigned long);
        case '?': return sizeof(bool);
        case 'c': return sizeof(char);
        case 'b': return sizeof(unsigned char);
        case 'x': return sizeof(unsigned char);
        default:  return 0;
    }
}

static bool IsPrintableString(const char* data, uint16_t count)
{
    for (uint16_t i = 0; i < count; i++)
    {
        if (data[i] == '\0') break;
        if (!isprint(static_cast<unsigned char>(data[i]))) return false;
    }
    return true;
}

static void PrintFieldValue(const char* data, char type, uint16_t count)
{
    if (type == 'x') { printf("-"); return; }

    if (type == 'c')
    {
        if (IsPrintableString(data, count))
            printf("%.*s", static_cast<int>(count), data);
        else
        {
            printf("0x");
            for (uint16_t i = 0; i < count; i++)
                printf("%02x", static_cast<unsigned char>(data[i]));
        }
        return;
    }

    size_t elemSize = TypeSize(type);
    for (uint16_t i = 0; i < count; i++)
    {
        const char* elem = data + i * elemSize;
        if (i > 0) printf(",");
        switch (type)
        {
            case 'i': { int v;           memcpy(&v, elem, 4);                       printf("%d",  v); break; }
            case 'I': { unsigned int v;  memcpy(&v, elem, 4);                       printf("%u",  v); break; }
            case 'l': { long v;          memcpy(&v, elem, sizeof(long));            printf("%ld", v); break; }
            case 'L': { unsigned long v; memcpy(&v, elem, sizeof(unsigned long));   printf("%lu", v); break; }
            case '?': { bool v;          memcpy(&v, elem, 1);                       printf("%s",  v ? "true" : "false"); break; }
            case 'b': { unsigned char v; memcpy(&v, elem, 1);                       printf("%u",  v); break; }
            default:  printf("?"); break;
        }
    }
}

static FieldDescriptor* GetDescriptors(char* base)
{
    return reinterpret_cast<FieldDescriptor*>(base + sizeof(DBHeader));
}

static char* GetRecordBase(char* base)
{
    DBHeader* hdr = reinterpret_cast<DBHeader*>(base);
    return base + sizeof(DBHeader) + hdr->m_SchemaSize;
}

static char* GetRecord(char* base, size_t record)
{
    DBHeader* hdr = reinterpret_cast<DBHeader*>(base);
    return GetRecordBase(base) + record * hdr->m_RecordSize;
}

static int ValidateDB(char* base, size_t fileSize)
{
    if (fileSize < sizeof(DBHeader))
    {
        fprintf(stderr, "not a qcDB file\n");
        return 1;
    }
    DBHeader* hdr = reinterpret_cast<DBHeader*>(base);
    uint32_t expected = hdr->m_NumFields * static_cast<uint32_t>(sizeof(FieldDescriptor));
    if (hdr->m_SchemaSize != expected)
    {
        fprintf(stderr, "corrupt schema block\n");
        return 1;
    }
    if (fileSize < sizeof(DBHeader) + hdr->m_SchemaSize)
    {
        fprintf(stderr, "corrupt schema block\n");
        return 1;
    }
    return 0;
}

static int CmdHeader(char* base)
{
    DBHeader* hdr = reinterpret_cast<DBHeader*>(base);
    printf("Object:   %s\n",   hdr->m_ObjectName);
    printf("Records:  %zu\n",  hdr->m_NumRecords);
    printf("Written:  %zu\n",  hdr->m_LastWritten);
    printf("RecordSz: %u bytes\n", hdr->m_RecordSize);
    printf("Fields:   %u\n",   hdr->m_NumFields);
    printf("SchemaSz: %u bytes\n", hdr->m_SchemaSize);
    return 0;
}

static int CmdSchema(char* base)
{
    DBHeader* hdr    = reinterpret_cast<DBHeader*>(base);
    FieldDescriptor* descs = GetDescriptors(base);

    printf("%-31s  TYPE  COUNT  OFFSET  FLAGS    VER\n",   "NAME");
    printf("%-31s  ----  -----  ------  -------  ---\n",
           "-------------------------------");

    for (uint8_t i = 0; i < hdr->m_NumFields; i++)
    {
        const char* flags = "";
        if ((descs[i].m_Flags & FIELD_FLAG_READONLY) &&
            (descs[i].m_Flags & FIELD_FLAG_METADATA))
            flags = "RO|META";
        else if (descs[i].m_Flags & FIELD_FLAG_READONLY)
            flags = "RO";
        else if (descs[i].m_Flags & FIELD_FLAG_METADATA)
            flags = "META";

        printf("%-31s  %-4c  %-5u  %-6u  %-7s  %u\n",
               descs[i].m_Name,
               descs[i].m_Type,
               descs[i].m_Count,
               descs[i].m_FieldOffset,
               flags,
               descs[i].m_SchemaVersion);
    }
    return 0;
}

static void PrintTableHeader(char* base)
{
    DBHeader* hdr    = reinterpret_cast<DBHeader*>(base);
    FieldDescriptor* descs = GetDescriptors(base);

    printf("DB: %s  |  Records: %zu  |  Written: %zu  |  RecordSz: %u bytes\n",
           hdr->m_ObjectName, hdr->m_NumRecords,
           hdr->m_LastWritten, hdr->m_RecordSize);

    for (uint8_t i = 0; i < hdr->m_NumFields; i++)
    {
        if (descs[i].m_Type == 'x') continue;
        printf("%-16s  ", descs[i].m_Name);
    }
    printf("\n");

    for (uint8_t i = 0; i < hdr->m_NumFields; i++)
    {
        if (descs[i].m_Type == 'x') continue;
        printf("----------------  ");
    }
    printf("\n");
}

static void PrintRecordTable(char* base, size_t record)
{
    DBHeader* hdr    = reinterpret_cast<DBHeader*>(base);
    FieldDescriptor* descs = GetDescriptors(base);
    char* rec = GetRecord(base, record);

    for (uint8_t i = 0; i < hdr->m_NumFields; i++)
    {
        if (descs[i].m_Type == 'x') continue;
        PrintFieldValue(rec + descs[i].m_FieldOffset,
                        descs[i].m_Type,
                        descs[i].m_Count);
        printf("  ");
    }
    printf("\n");
}

static int CmdDump(char* base, size_t recordStart, size_t recordEnd, bool hex)
{
    DBHeader* hdr = reinterpret_cast<DBHeader*>(base);

    if (recordStart >= hdr->m_NumRecords)
    {
        fprintf(stderr, "record %zu out of range (max %zu)\n",
                recordStart, hdr->m_NumRecords - 1);
        return 1;
    }
    if (recordEnd > hdr->m_NumRecords) recordEnd = hdr->m_NumRecords;

    if (!hex)
    {
        PrintTableHeader(base);
        for (size_t r = recordStart; r < recordEnd; r++)
            PrintRecordTable(base, r);
    }
    else
    {
        for (size_t r = recordStart; r < recordEnd; r++)
        {
            char* rec = GetRecord(base, r);
            printf("Record %zu:\n", r);
            for (uint32_t b = 0; b < hdr->m_RecordSize; b++)
            {
                if (b % 16 == 0) printf("  %04x: ", b);
                printf("%02x ", static_cast<unsigned char>(rec[b]));
                if (b % 16 == 15) printf("\n");
            }
            printf("\n");
        }
    }
    return 0;
}

static int CmdWrite(char* base, size_t record,
                    const char* fieldName, const char* value)
{
    DBHeader* hdr    = reinterpret_cast<DBHeader*>(base);
    FieldDescriptor* descs = GetDescriptors(base);

    if (record >= hdr->m_NumRecords)
    {
        fprintf(stderr, "record %zu out of range (max %zu)\n",
                record, hdr->m_NumRecords - 1);
        return 1;
    }

    int fieldIdx = -1;
    for (uint8_t i = 0; i < hdr->m_NumFields; i++)
    {
        if (strncmp(descs[i].m_Name, fieldName, sizeof(descs[i].m_Name)) == 0)
        {
            fieldIdx = i;
            break;
        }
    }
    if (fieldIdx < 0)
    {
        fprintf(stderr, "unknown field: %s\n", fieldName);
        return 1;
    }

    const FieldDescriptor& desc = descs[fieldIdx];

    if (desc.m_Flags & FIELD_FLAG_READONLY)
    {
        fprintf(stderr, "field %s is read-only\n", fieldName);
        return 1;
    }

    char writeBuf[65536] = {};
    size_t fieldBytes = desc.m_Count * TypeSize(desc.m_Type);

    if (desc.m_Type == 'c')
    {
        size_t len = strlen(value);
        if (len >= desc.m_Count)
        {
            fprintf(stderr, "value too long (max %u chars)\n",
                    static_cast<unsigned>(desc.m_Count - 1));
            return 1;
        }
        strncpy(writeBuf, value, desc.m_Count);
    }
    else
    {
        char* end = nullptr;
        switch (desc.m_Type)
        {
            case 'i': {
                long v = strtol(value, &end, 10);
                if (*end || v < INT_MIN || v > INT_MAX)
                { fprintf(stderr, "value out of range for type i\n"); return 1; }
                int iv = static_cast<int>(v);
                memcpy(writeBuf, &iv, 4);
                break;
            }
            case 'I': {
                unsigned long v = strtoul(value, &end, 10);
                if (*end || v > UINT_MAX)
                { fprintf(stderr, "value out of range for type I\n"); return 1; }
                unsigned int iv = static_cast<unsigned int>(v);
                memcpy(writeBuf, &iv, 4);
                break;
            }
            case 'l': {
                long v = strtol(value, &end, 10);
                if (*end) { fprintf(stderr, "value out of range for type l\n"); return 1; }
                memcpy(writeBuf, &v, sizeof(long));
                break;
            }
            case 'L': {
                unsigned long v = strtoul(value, &end, 10);
                if (*end) { fprintf(stderr, "value out of range for type L\n"); return 1; }
                memcpy(writeBuf, &v, sizeof(unsigned long));
                break;
            }
            case '?': {
                if (strcmp(value, "true") == 0 || strcmp(value, "1") == 0)
                    writeBuf[0] = 1;
                else if (strcmp(value, "false") == 0 || strcmp(value, "0") == 0)
                    writeBuf[0] = 0;
                else { fprintf(stderr, "value out of range for type ?\n"); return 1; }
                break;
            }
            case 'b': {
                unsigned long v = strtoul(value, &end, 10);
                if (*end || v > 255)
                { fprintf(stderr, "value out of range for type b\n"); return 1; }
                writeBuf[0] = static_cast<unsigned char>(v);
                break;
            }
            default:
                fprintf(stderr, "value out of range for type %c\n", desc.m_Type);
                return 1;
        }
    }

    if (pthread_rwlock_wrlock(&hdr->m_DBLock) != 0)
    {
        fprintf(stderr, "failed to acquire write lock\n");
        return 1;
    }

    char* rec = GetRecord(base, record);
    memcpy(rec + desc.m_FieldOffset, writeBuf, fieldBytes);

    for (uint8_t i = 0; i < hdr->m_NumFields; i++)
    {
        if (strcmp(descs[i].m_Name, "LAST_MODIFIED") == 0)
        {
            struct timespec ts = {};
            clock_gettime(CLOCK_REALTIME, &ts);
            unsigned long ns =
                static_cast<unsigned long>(ts.tv_sec) * 1000000000UL +
                static_cast<unsigned long>(ts.tv_nsec);
            memcpy(rec + descs[i].m_FieldOffset, &ns, sizeof(unsigned long));
            break;
        }
    }

    pthread_rwlock_unlock(&hdr->m_DBLock);
    printf("wrote %s[%zu].%s = %s\n", hdr->m_ObjectName, record, fieldName, value);
    return 0;
}

static void Usage(const char* prog)
{
    fprintf(stderr,
        "usage: %s <file.db> [options]\n"
        "  (no options)               dump all records, table format\n"
        "  --record <N>               dump single record N\n"
        "  --records <N-M>            dump record range N through M\n"
        "  --header                   show DB header info\n"
        "  --schema                   show field descriptor table\n"
        "  --write <N> <field> <val>  write field value on record N\n"
        "  --format <table|hex>       output format (default: table)\n",
        prog);
}

int main(int argc, char** argv)
{
    if (argc < 2) { Usage(argv[0]); return 1; }

    const char* dbPath = argv[1];

    bool doHeader  = false;
    bool doSchema  = false;
    bool doWrite   = false;
    bool formatHex = false;
    bool needWrite = false;
    size_t recordStart = 0;
    size_t recordEnd   = SIZE_MAX;
    size_t writeRecord = 0;
    const char* writeField = nullptr;
    const char* writeValue = nullptr;

    for (int i = 2; i < argc; i++)
    {
        if (strcmp(argv[i], "--header") == 0)
        {
            doHeader = true;
        }
        else if (strcmp(argv[i], "--schema") == 0)
        {
            doSchema = true;
        }
        else if (strcmp(argv[i], "--record") == 0 && i + 1 < argc)
        {
            recordStart = static_cast<size_t>(atoll(argv[++i]));
            recordEnd   = recordStart + 1;
        }
        else if (strcmp(argv[i], "--records") == 0 && i + 1 < argc)
        {
            size_t n1 = 0, n2 = 0;
            if (sscanf(argv[++i], "%zu-%zu", &n1, &n2) != 2)
            {
                fprintf(stderr, "--records requires N-M format\n");
                return 1;
            }
            recordStart = n1;
            recordEnd   = n2 + 1;
        }
        else if (strcmp(argv[i], "--write") == 0 && i + 3 < argc)
        {
            doWrite    = true;
            needWrite  = true;
            writeRecord = static_cast<size_t>(atoll(argv[++i]));
            writeField  = argv[++i];
            writeValue  = argv[++i];
        }
        else if (strcmp(argv[i], "--format") == 0 && i + 1 < argc)
        {
            const char* fmt = argv[++i];
            if (strcmp(fmt, "hex") == 0) formatHex = true;
        }
        else
        {
            fprintf(stderr, "unknown option: %s\n", argv[i]);
            Usage(argv[0]);
            return 1;
        }
    }

    int flags = needWrite ? O_RDWR : O_RDONLY;
    int fd = open(dbPath, flags);
    if (fd < 0)
    {
        fprintf(stderr, "cannot open %s: %s\n", dbPath, strerror(errno));
        return 1;
    }

    struct stat st;
    if (fstat(fd, &st) < 0)
    {
        close(fd);
        fprintf(stderr, "cannot open %s\n", dbPath);
        return 1;
    }
    size_t fileSize = static_cast<size_t>(st.st_size);

    int prot = needWrite ? (PROT_READ | PROT_WRITE) : PROT_READ;
    char* base = static_cast<char*>(
        mmap(nullptr, fileSize, prot, MAP_SHARED, fd, 0));
    close(fd);

    if (MAP_FAILED == base)
    {
        fprintf(stderr, "cannot map %s: %s\n", dbPath, strerror(errno));
        return 1;
    }

    int rc = ValidateDB(base, fileSize);
    if (rc != 0) { munmap(base, fileSize); return rc; }

    if      (doHeader) rc = CmdHeader(base);
    else if (doSchema) rc = CmdSchema(base);
    else if (doWrite)  rc = CmdWrite(base, writeRecord, writeField, writeValue);
    else               rc = CmdDump(base, recordStart, recordEnd, formatHex);

    munmap(base, fileSize);
    return rc;
}
