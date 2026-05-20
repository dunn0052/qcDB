#include <common/CLI.hh>
#include <common/Logger.hh>

#include <qcDB/qcDB.hh>
#include <dbHeaders/CHARACTER.hh>

#include <chrono>
#include <sys/mman.h>
#include <sys/wait.h>
#include <sys/shm.h>
#include <random>
#include <algorithm>

int64_t g_TIME = 1;
int g_NUM_PROCESSES = 4;
size_t g_NUM_ALTERED_RECORDS = 20;
std::string g_NAME = "KEVIN";

static void DBWriters(const std::string& dbPath, long long totals[], int index)
{
    RETCODE retcode = RTN_OK;
    qcDB::dbInterface<CHARACTER> database(dbPath);

    // Set up RNG
    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_int_distribution<int> distribution(0, database.NumberOfRecords() - 1);

    std::vector<std::tuple<size_t, CHARACTER>> characters;

    for(int charIndex = 0; charIndex < g_NUM_ALTERED_RECORDS; charIndex++)
    {
        int record = distribution(gen);
        CHARACTER character = { 0 };
        character.AGE = index;
        character.RECORD = record;
        strcpy(character.NAME, g_NAME.c_str());
        characters.push_back(std::tuple<size_t, CHARACTER>(record, character));
    }

    bool running = true;
    std::chrono::_V2::steady_clock::time_point start = std::chrono::steady_clock::now();
    while(running)
    {
        retcode = database.WriteObjects(characters);
        if(RTN_OK != retcode)
        {
            LOG_WARN("Error writing characters for process: ",index ," with error: ", retcode);
            return;
        }

        (*totals) += g_NUM_ALTERED_RECORDS;
        if(std::chrono::steady_clock::now() - start > std::chrono::seconds(g_TIME))
        {
            running = false;
        }
    }

    LOG_DEBUG("Wrote: ", *totals, " records");
}

static void DBReader(const std::string& dbPath, long long totals[], int index)
{
    RETCODE retcode = RTN_OK;

    qcDB::dbInterface<CHARACTER> database(dbPath);

    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_int_distribution<int> distribution(0, database.NumberOfRecords() - 1);

    std::vector<std::tuple<size_t, CHARACTER>> characters;

    for(int charIndex = 0; charIndex < g_NUM_ALTERED_RECORDS; charIndex++)
    {
        CHARACTER character = { 0 };
        characters.push_back(std::tuple<size_t, CHARACTER>(distribution(gen), character));
    }

    bool running = true;
    std::chrono::_V2::steady_clock::time_point start = std::chrono::steady_clock::now();
    while(running)
    {
        retcode = database.ReadObjects(characters);
        if(RTN_OK != retcode)
        {
            LOG_WARN("Error reading characters for process: ", index, " with error: ", retcode);
            return;
        }

        (*totals) += g_NUM_ALTERED_RECORDS;
        if(std::chrono::steady_clock::now() - start > std::chrono::seconds(g_TIME))
        {
            running = false;
        }
    }

    LOG_DEBUG("Read: ", *totals, " records");

}

int main(int argc, char* argv[])
{
    CLI_StringArgument dbPathArg("-d", "The path to the CHARACTER database file", true);
    CLI_IntArgument timeArg("-s", "Number of seconds to run the test");
    CLI_IntArgument numProcessArg("-p", "Number of each read and write processes");
    CLI_IntArgument numRecordAltsArg("-r", "Number of altered records");
    CLI_StringArgument nameArg("-n", "Character name");

    Parser parser("charTEST", "Test CHARACTER database");

    parser
        .AddArg(dbPathArg)
        .AddArg(timeArg)
        .AddArg(numProcessArg)
        .AddArg(numRecordAltsArg)
        .AddArg(nameArg);

    RETCODE retcode = parser.ParseCommandLineArguments(argc, argv);

    if(RTN_OK != retcode)
    {
        parser.Usage();
        return retcode;
    }

    qcDB::dbInterface<CHARACTER> database(dbPathArg.GetValue());

    if(timeArg.IsInUse())
    {
        g_TIME = timeArg.GetValue();
    }

    if(numProcessArg.IsInUse())
    {
        g_NUM_PROCESSES = numProcessArg.GetValue();
    }

    // One process for a read and one for write
    g_NUM_PROCESSES *= 2;

    if(numRecordAltsArg.IsInUse())
    {
        g_NUM_ALTERED_RECORDS = numRecordAltsArg.GetValue();
    }

    if(nameArg.IsInUse())
    {
        g_NAME = nameArg.GetValue();
    }

    /// Set up shared memory key on a file
    key_t key = ftok("/home/kdunn/.bashrc", 1);
    if(-1 == key)
    {
        LOG_FATAL("Could not get shared memory key due to error: ", ErrorString(errno));
        return RTN_NULL_OBJ;
    }

    // Create an array of long long for totals of each write/read process
    int shm_id = shmget(key, g_NUM_PROCESSES * sizeof(long long), 0666 | IPC_CREAT);;
    if(-1 == shm_id)
    {
        LOG_FATAL("Could not create shared memory array due to error: ", ErrorString(errno));
        return RTN_MALLOC_FAIL;
    }

    // Get a pointer to the shared memory array
    long long* totals = static_cast<long long*>(shmat(shm_id, NULL, 0));
    if(nullptr == totals)
    {
        LOG_FATAL("Could not get shared memory array due to error: ", ErrorString(errno));
        return RTN_NULL_OBJ;
    }

    // Ensure that all entries start at 0 (probably not actually needed)
    memset(totals, 0, g_NUM_PROCESSES * sizeof(long long));

    int* pids = new int[g_NUM_PROCESSES];

    for(int processIndex = 0; processIndex < g_NUM_PROCESSES; processIndex++)
    {
        if(processIndex % 2)
        {
            pids[processIndex] = fork();
            if(pids[processIndex] == 0)
            {
                DBReader(dbPathArg.GetValue(), &totals[processIndex], processIndex);
                return 0;
            }
            else
            {
                continue;
            }
        }
        else
        {
            pids[processIndex] = fork();
            if(pids[processIndex] == 0)
            {
                DBWriters(dbPathArg.GetValue(), &totals[processIndex], processIndex);
                return 0;
            }
            else
            {
                continue;
            }
        }
    }

    for (int processIndex = 0; processIndex < g_NUM_PROCESSES; ++processIndex)
    {
        int status;
        while (-1 == waitpid(pids[processIndex], &status, 0));
        if (!WIFEXITED(status) || WEXITSTATUS(status) != 0)
        {
            std::cerr << "Process " << processIndex << " (pid " << pids[processIndex] << ") failed\n";
            exit(1);
        }
    }

    long long totalReads = 0;
    long long totalWrites = 0;

    for(int processIndex = 0; processIndex < g_NUM_PROCESSES; processIndex++)
    {
        if(processIndex % 2)
        {
            totalReads += totals[processIndex];
        }
        else
        {
            totalWrites += totals[processIndex];

        }
    }

    LOG_INFO("Total reads: ", totalReads, " total writes: ", totalWrites);
    LOG_INFO("Total reads + writes: ", totalReads + totalWrites, " in: ", g_TIME, " second(s)");

    shmctl(shm_id, IPC_RMID, 0);

    std::vector<CHARACTER> foundRecords;
    retcode = database.FindObjects(
        [](const CHARACTER* character) -> bool
        {
            return !strcmp(character->NAME, "KEVIN");
        },
        foundRecords
    );

    if(RTN_OK != retcode)
    {
        LOG_WARN("Failed to find due to error: ", retcode);
        return retcode;
    }

    double percent = static_cast<double>(foundRecords.size()) / database.NumberOfRecords() * 100;
    LOG_INFO("Found: ", foundRecords.size(), " matching records ", foundRecords.size(), "/", database.NumberOfRecords(), " (", percent, "%)");

    return retcode;
}