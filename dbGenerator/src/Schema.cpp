#include <common/OSdefines.hh>
#include <dbGenerator/inc/Schema.hh>
#include <dbGenerator/inc/ObjectSchema.hh>
#include <common/Logger.hh>
#include <common/Constants.hh>
#include <common/UtilityFunctions.hh>
#include <common/DBHeader.hh>

#include <fcntl.h>
#include <fstream>

static RETCODE ParseField(std::istringstream& lineStream, FIELD_SCHEMA& out_field)
{
    lineStream >> out_field;
    if(lineStream.fail())
    {
        return RTN_BAD_ARG;
    }

    switch(static_cast<FIELD_TYPE>(out_field.fieldType))
    {
        case FIELD_TYPE::INT:
        {
            out_field.fieldSize = sizeof(int);
            out_field.fieldAlignment = alignof(int);
            break;
        }
        case FIELD_TYPE::UINT:
        {
            out_field.fieldSize = sizeof(unsigned int);
            out_field.fieldAlignment = alignof(unsigned int);
            break;
        }
        case FIELD_TYPE::LONG:
        {
            out_field.fieldSize = sizeof(long);
            out_field.fieldAlignment = alignof(long);
            break;
        }
        case FIELD_TYPE::ULONG:
        {
            out_field.fieldSize = sizeof(unsigned long);
            out_field.fieldAlignment = alignof(unsigned long);
            break;
        }
        case FIELD_TYPE::CHAR:
        {
            out_field.fieldSize = sizeof(char);
            out_field.fieldAlignment = alignof(char);
            break;
        }
#ifdef WINDOWS_PLATFORM
        case FIELD_TYPE::WCHAR:
        {
            out_field.fieldSize = sizeof(wchar_t);
            out_field.fieldAlignment = alignof(wchar_t);
            break;
        }
#endif
        case FIELD_TYPE::BYTE:
        {
            out_field.fieldSize = sizeof(unsigned char);
            out_field.fieldAlignment = alignof(unsigned char);
            break;
        }
        case FIELD_TYPE::BOOL:
        {
            out_field.fieldSize = sizeof(bool);
            out_field.fieldAlignment = alignof(bool);
            break;
        }
        case FIELD_TYPE::PADDING:
        {
            out_field.fieldSize = sizeof(unsigned char);
            out_field.fieldAlignment = alignof(unsigned char);
            break;
        }
        default:
        {
            LOG_FATAL("field: ",
                out_field.fieldName,
                " type: ",
                out_field.fieldType,
                " is invalid");

            return RTN_BAD_ARG;
        }
    }

    out_field.fieldSize *= out_field.numElements;

    LOG_INFO("FIELD NUMBER: ",
        out_field.fieldNumber,
        " FIELD NAME: ",
        out_field.fieldName,
        " FIELD TYPE: ",
        out_field.fieldType,
        " NUMBER OF ELEMENTS: ",
        out_field.numElements);

    return RTN_OK;
}

static RETCODE ParseObject(std::istringstream& lineStream, OBJECT_SCHEMA& out_object)
{
    lineStream >> out_object;
    if(lineStream.fail())
    {
        return RTN_BAD_ARG;
    }

    LOG_INFO("OBJECT NUMBER: ",
        out_object.objectNumber,
        " OBJECT NAME: ",
        out_object.objectName,
        " NUMBER OF RECORDS: ",
        out_object.numberOfRecords);

    return RTN_OK;
}

static RETCODE GenerateObjectHeader(const OBJECT_SCHEMA& object, std::ofstream& headerFile)
{
    headerFile << "// GENERATED: DO NOT MODIFY!!\n";
    /* Header guard */
    headerFile << "#ifndef " << std::uppercase << object.objectName << "__HH\n";
    headerFile << "#define " << std::uppercase << object.objectName << "__HH\n\n";

    headerFile
        << "\nstruct "
        << std::uppercase
        << object.objectName
        << "\n{";

    if(headerFile.bad())
    {
        LOG_FATAL("Could not write object: ",
            object.objectName,
            " due to error: ",
            ErrorString(errno));

        return RTN_FAIL;
    }

    return RTN_OK;
}

static RETCODE GenerateFieldHeader(const FIELD_SCHEMA& field, std::ofstream& headerFile)
{
    std::string dataType;
    switch(static_cast<FIELD_TYPE>(field.fieldType))
    {
        case FIELD_TYPE::INT:
        {
            dataType = "int";
            break;
        }
        case FIELD_TYPE::UINT:
        {
            dataType = "unsigned int";
            break;
        }
        case FIELD_TYPE::LONG:
        {
            dataType = "long";
            break;
        }
        case FIELD_TYPE::ULONG:
        {
            dataType = "unsigned long";
            break;
        }
        case FIELD_TYPE::CHAR:
        {
            dataType = "char";
            break;
        }
#ifdef WINDOWS_PLATFORM
        case FIELD_TYPE::WCHAR:
        {
            dataType = "wchar_t";
            break;
        }
#endif
        case FIELD_TYPE::BYTE:
        {
            dataType = "unsigned char";
            break;
        }
        case FIELD_TYPE::BOOL:
        {
            dataType = "bool";
            break;
        }
        case FIELD_TYPE::PADDING:
        {
            dataType = "unsigned char";
            break;
        }
        default:
        {
            LOG_FATAL("field: ",
                field.fieldName,
                " type: ",
                field.fieldType,
                " is invalid");

            return RTN_BAD_ARG;
        }
    }

    if(field.numElements > 1)
    {
        /* Array of elements */
        headerFile
            << "\n    "
            << dataType
            << " "
            << field.fieldName
            << "["
            << field.numElements
            << "];";
    }
    else
    {
        headerFile
            << "\n    "
            << dataType
            << " "
            << field.fieldName
            << ";";
    }

    if(headerFile.bad())
    {
        LOG_FATAL("Could not generate header file entry for field: ",
            field.fieldName,
            " due to error: ",
            ErrorString(errno));

        return RTN_FAIL;
    }

    return RTN_OK;
}

static RETCODE GenerateObjectFooter(const OBJECT_SCHEMA& object, std::ofstream& headerFile)
{
    headerFile << "\n};\n\n";

    headerFile << "#endif";

    if(headerFile.bad())
    {
        LOG_FATAL("Could not generate footer for header file: ",
            object.objectName,
            " due to error: ",
            ErrorString(errno));
        return RTN_FAIL;
    }

    return RTN_OK;
}

static RETCODE GenerateHeader(const OBJECT_SCHEMA& object, const std::string& headerOutputDirectory)
{
    RETCODE retcode = RTN_OK;
    std::string headerFile = headerOutputDirectory + object.objectName + CONSTANTS::HEADER_EXT;

    std::ofstream headerFileStream(headerFile);
    if(!headerFileStream)
    {
        LOG_FATAL("Could not create: ",
            headerFile,
            " due to error: ",
            ErrorString(errno));

        return RTN_NOT_FOUND;
    }

    retcode = GenerateObjectHeader(object, headerFileStream);
    if(RTN_OK != retcode)
    {
        return retcode;
    }

    for(const FIELD_SCHEMA& field : object.fields)
    {
        retcode = GenerateFieldHeader(field, headerFileStream);
        if(RTN_OK != retcode)
        {
            return retcode;
        }
    }

    retcode = GenerateObjectFooter(object, headerFileStream);
    if(RTN_OK != retcode)
    {
        return retcode;
    }

    LOG_INFO("Generated: ", headerFile);

    return RTN_OK;
}

size_t inline CalculatePadding(const OBJECT_SCHEMA& object, const FIELD_SCHEMA& field)
{
    size_t fieldOffset = object.objectSize % field.fieldAlignment;
    size_t padding = (field.fieldAlignment - fieldOffset) % field.fieldAlignment;
    return padding;
}

RETCODE CreateDatabaseFile(const OBJECT_SCHEMA& object, const std::string& databaseOutputDirectory)
{
    std::string databaseFile = databaseOutputDirectory + object.objectName + CONSTANTS::DB_EXT;
    size_t fileSize = sizeof(DBHeader) + object.objectSize * object.numberOfRecords;

    LOG_DEBUG(databaseFile, " is: ", fileSize, " bytes");

#ifdef WINDOWS_PLATFORM
    HANDLE hFile = CreateFileA(databaseFile.c_str(),
                               GENERIC_READ | GENERIC_WRITE,
                               0,
                               NULL,
                               OPEN_ALWAYS,
                               FILE_ATTRIBUTE_NORMAL,
                               NULL);
    LARGE_INTEGER li;
    li.QuadPart = fileSize;
    if (!SetFilePointerEx(hFile, li, NULL, FILE_BEGIN))
    {
        LOG_FATAL("Could not set file pointer for file: ",
            databaseFile,
            " due to error: ",
            ErrorString(GetLastError()));
        CloseHandle(hFile);
        return RTN_MALLOC_FAIL;
    }

    // Truncate the file to the current position, effectively setting the file size
    if (!SetEndOfFile(hFile))
    {
        LOG_FATAL("Failed to truncate file: ",
            databaseFile,
            " due to error: ",
            ErrorString(GetLastError()));
        CloseHandle(hFile);
        return RTN_EOF;
    }

    li.QuadPart = 0;
    if (!SetFilePointerEx(hFile, li, NULL, FILE_BEGIN))
    {
        LOG_FATAL("Could not reset pointer for file: ",
            databaseFile,
            " due to error: ",
            ErrorString(GetLastError()));
        CloseHandle(hFile);
        return RTN_MALLOC_FAIL;
    }
#else
    int fd = open(databaseFile.c_str(), O_RDWR | O_CREAT, CONSTANTS::RW);
    if( 0 > fd )
    {
        LOG_WARN("Failed to open or create ", databaseFile);
        return  RTN_NOT_FOUND;
    }

    if(ftruncate64(fd, fileSize))
    {
        LOG_WARN("Failed to truncate ", databaseFile, " to size ", fileSize);
        close(fd);
        return RTN_MALLOC_FAIL;
    }
#endif

    DBHeader dbHeader = { 0 };
    dbHeader.m_NumRecords = object.numberOfRecords;

#ifdef WINDOWS_PLATFORM

    int error = strncpy_s(dbHeader.m_ObjectName, object.objectName.c_str(), object.objectName.length());
    if(error)
    {
        LOG_FATAL("Could not write object name: ",
            object.objectName,
            " to DBHeader due to error: ",
            ErrorString(error));
    }

    DWORD numbytes = 0;
    if (!WriteFile(hFile, &dbHeader, sizeof(DBHeader), &numbytes, NULL))
    {
        LOG_FATAL("Failed to write DBHeader to file: ",
            databaseFile,
            " due to error: ",
            ErrorString(errno));
        CloseHandle(hFile);
        return RTN_EOF;
    }

    if(sizeof(DBHeader) != numbytes)
    {
        LOG_FATAL("Could not write DBHeader for: ",
            object.objectName);
        CloseHandle(hFile);
        return RTN_EOF;
    }
#else

    strncpy(dbHeader.m_ObjectName, object.objectName.c_str(), object.objectName.length());

    pthread_rwlockattr_t dbLockAttributest = {0};
    pthread_rwlockattr_init(&dbLockAttributest);
    pthread_rwlockattr_setpshared(&dbLockAttributest, PTHREAD_PROCESS_SHARED);
    pthread_rwlock_init(&dbHeader.m_DBLock, &dbLockAttributest);
    pthread_rwlockattr_destroy(&dbLockAttributest);
    size_t numbytes = write(fd, static_cast<void*>(&dbHeader), sizeof(DBHeader));

    if(sizeof(DBHeader) != numbytes)
    {
        close(fd);
        LOG_FATAL("Failed to create DB header for: ", object.objectName);
        return RTN_EOF;
    }
#endif

#ifdef WINDOWS_PLATFORM
    if(!CloseHandle(hFile))
    {
        LOG_WARN("Failed to close ",
            databaseFile,
            " due to error: ",
            ErrorString(GetLastError()));
        return RTN_FAIL;
    }
#else
    if(close(fd))
    {
        LOG_WARN("Failed to close ", databaseFile);
        return RTN_FAIL;
    }
#endif


    LOG_INFO("Generated: ", databaseFile);

    return RTN_OK;
}

RETCODE GenerateDatabase(const std::string& schemaPath, const std::string& headerOutputPath, const std::string& databaseOutputPath, bool isStrict)
{
    RETCODE retcode = RTN_OK;
    size_t currentLineNumber = 0;
    size_t firstNonEmptyChar = 0;
    char firstChar = 0;
    OBJECT_SCHEMA object = { 0 };
    bool readObject = false;

    std::ifstream schema(schemaPath);
    if(!schema)
    {
        int error = errno;
        LOG_FATAL("Failed to open: ",
            schemaPath,
            " due to error: ",
            ErrorString(error));
        return RTN_NOT_FOUND;
    }

    std::string line;
    while(std::getline(schema, line))
    {
        currentLineNumber++;
        firstNonEmptyChar = line.find_first_not_of(" \t");

        if(std::string::npos == firstNonEmptyChar)
        {
            continue;
        }

        firstChar = line[firstNonEmptyChar];
        if(CONSTANTS::SCHEMA_COMMENT == firstChar)
        {
            continue;
        }

        std::istringstream lineStream(line);
        if(readObject)
        {
            FIELD_SCHEMA field = { 0 };
            retcode = ParseField(lineStream, field);
            if(RTN_OK != retcode)
            {
                LOG_FATAL("Error parsing field on line: ", currentLineNumber);
                return retcode;
            }

            size_t padding = CalculatePadding(object, field);
            if(isStrict)
            {
                if(padding)
                {
                    LOG_FATAL("Padding of: ",
                        padding,
                        " bytes detected for field: ",
                        field.fieldName,
                        " on line: ",
                        currentLineNumber);

                    return RTN_BAD_ARG;
                }
            }

            object.objectSize += field.fieldSize + padding;
            object.fields.push_back(field);
        }
        else
        {
            retcode = ParseObject(lineStream, object);
            if(RTN_OK != retcode)
            {
                LOG_FATAL("Error parsing object on line: ", currentLineNumber);
                return retcode;
            }

            readObject = true;
        }
    }

    LOG_DEBUG("OBJECT: ", object.objectName, " size is: ", object.objectSize, " bytes per record");

    retcode = GenerateHeader(object, headerOutputPath);
    if(RTN_OK != retcode)
    {
        return retcode;
    }

    retcode = CreateDatabaseFile(object, databaseOutputPath);
    if(RTN_OK != retcode)
    {
        return retcode;
    }

    return retcode;
}
