#include <common/CLI.hh>
#include <qcDB/qcDB.hh>
#include <dbHeaders/index.hh>

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

    if (RTN_OK != retcode)
    {
        parser.Usage();
        return retcode;
    }
    
    qcDB::dbInterface<INDEX> database(dbPathArg.GetValue());

    return retcode;
}