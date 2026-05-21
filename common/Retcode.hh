#ifndef RETCODE__HH
#define RETCODE__HH

    // Return code
    typedef unsigned int RETCODE;

    // Retcode types

    // Everything was ok
    constexpr RETCODE RTN_OK = 0x0000;

    // General failure
    constexpr RETCODE RTN_FAIL = 0x0001;

    // Encountered a null object
    constexpr RETCODE RTN_NULL_OBJ = 0x0002;

    // Memory allocation error
    constexpr RETCODE RTN_MALLOC_FAIL = 0x0004;

    // Could not find object
    constexpr RETCODE RTN_NOT_FOUND = 0x0008;

    // Problem with network connection
    constexpr RETCODE RTN_CONNECTION_FAIL = 0x0010;

    // End of File
    constexpr RETCODE RTN_EOF = 0x0020;

    // Bad argument
    constexpr RETCODE RTN_BAD_ARG = 0x0040;

    // Timeout
    constexpr RETCODE RTN_TIMEOUT = 0x0080;

    // Lock failed
    constexpr RETCODE RTN_LOCK_ERROR = 0x0100;

    // Record size in file does not match sizeof(T)
    constexpr RETCODE RTN_SIZE_MISMATCH = 0x0200;

    // Schema block invariant violated — file may be corrupt
    constexpr RETCODE RTN_CORRUPT       = 0x0400;

#endif