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
static bool TestWriteObjects(const char* path);
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
    pass &= TestWriteObjects(dbPath);
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
    db.Clear();
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
    db.Clear();

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
    db.Clear();

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

static bool TestWriteObjects(const char* path)
{
    qcDB::dbInterface<TestRecord> db(path);
    db.Clear();

    TestRecord a = {}; a.id = 11; strncpy(a.name, "WOBJ_A", 28);
    TestRecord b = {}; b.id = 22; strncpy(b.name, "WOBJ_B", 28);
    TestRecord c = {}; c.id = 33; strncpy(c.name, "WOBJ_C", 28);
    std::vector<TestRecord> toWrite = {a, b, c};

    TEST_ASSERT(RTN_OK == db.WriteObjects(toWrite), "WriteObjects failed");

    size_t found = 0;
    TEST_ASSERT(RTN_OK == db.FindFirstOf([](const TestRecord* r){ return r->id == 11; }, found),
        "FindFirstOf id=11 not found after WriteObjects");
    TEST_ASSERT(RTN_OK == db.FindFirstOf([](const TestRecord* r){ return r->id == 22; }, found),
        "FindFirstOf id=22 not found after WriteObjects");
    TEST_ASSERT(RTN_OK == db.FindFirstOf([](const TestRecord* r){ return r->id == 33; }, found),
        "FindFirstOf id=33 not found after WriteObjects");

    fprintf(stdout, "PASS: TestWriteObjects\n");
    return true;
}

static bool TestFindFirstOf(const char* path)
{
    qcDB::dbInterface<TestRecord> db(path);
    db.Clear();

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
    db.Clear();

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
