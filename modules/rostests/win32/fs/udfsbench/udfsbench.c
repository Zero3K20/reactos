/*
 * ReactOS UDFS write and metadata benchmark.
 */

#include <stdio.h>
#include <stdlib.h>
#include <wchar.h>
#define WIN32_NO_STATUS
#include <windows.h>
#define NTOS_MODE_USER
#include <ndk/ntndk.h>

#define DEFAULT_FILE_COUNT 100
#define DEFAULT_FILE_SIZE 4096
#define DEFAULT_REPEATS 3
#define MAX_FILE_COUNT 1024
#define MAX_FILE_SIZE (1024 * 1024)
#define MAX_TOTAL_BYTES (256ULL * 1024 * 1024)
#define MAX_PATH_LENGTH  MAX_PATH

typedef struct _BENCHMARK_RESULT
{
    LARGE_INTEGER CreateTime;
    LARGE_INTEGER WriteTime;
    LARGE_INTEGER MetadataTime;
    LARGE_INTEGER FlushTime;
    LARGE_INTEGER CloseTime;
} BENCHMARK_RESULT;

typedef struct _BENCHMARK_CONFIG
{
    DWORD FileCount;
    DWORD FileSize;
    DWORD Repeats;
    BOOL KeepFiles;
} BENCHMARK_CONFIG;

static BOOL
AppendString(WCHAR *Destination, SIZE_T Capacity, PCWSTR Source)
{
    SIZE_T DestinationLength = wcslen(Destination);
    SIZE_T SourceLength = wcslen(Source);

    if (DestinationLength + SourceLength + 1 > Capacity)
        return FALSE;

    wcscpy(Destination + DestinationLength, Source);
    return TRUE;
}

static BOOL
AppendFormat(WCHAR *Destination, SIZE_T Capacity, PCWSTR Format, DWORD Value)
{
    WCHAR Buffer[64];
    int Length = _snwprintf(Buffer, sizeof(Buffer) / sizeof(Buffer[0]) - 1,
                            Format, Value);

    if (Length < 0)
        return FALSE;
    Buffer[Length] = L'\0';
    return AppendString(Destination, Capacity, Buffer);
}

static BOOL
ParseNumber(PCWSTR String, DWORD Minimum, DWORD Maximum, DWORD *Value)
{
    WCHAR *End;
    unsigned long Parsed;

    if (!String[0] || String[0] == L'-')
        return FALSE;

    Parsed = wcstoul(String, &End, 10);
    if (*End || Parsed < Minimum || Parsed > Maximum)
        return FALSE;

    *Value = (DWORD)Parsed;
    return TRUE;
}

static double
Milliseconds(LONGLONG Ticks, LONGLONG Frequency)
{
    return (double)Ticks * 1000.0 / (double)Frequency;
}

static ULONGLONG
FileTimeToInteger(const FILETIME *FileTime)
{
    return ((ULONGLONG)FileTime->dwHighDateTime << 32) |
           FileTime->dwLowDateTime;
}

static BOOL
CreateRunDirectory(PCWSTR ParentPath, WCHAR *RunPath, SIZE_T RunPathCapacity)
{
    DWORD Attempt;
    DWORD ParentLength;
    DWORD PathLength;

    PathLength = GetFullPathNameW(ParentPath, (DWORD)RunPathCapacity,
                                  RunPath, NULL);
    if (!PathLength || PathLength >= RunPathCapacity)
        return FALSE;

    ParentLength = (DWORD)wcslen(RunPath);
    if (ParentLength && RunPath[ParentLength - 1] != L'\\' &&
        RunPath[ParentLength - 1] != L'/')
    {
        if (!AppendString(RunPath, RunPathCapacity, L"\\"))
            return FALSE;
    }

    for (Attempt = 0; Attempt < 16; ++Attempt)
    {
        WCHAR Candidate[MAX_PATH_LENGTH];
        DWORD Length;

        wcscpy(Candidate, RunPath);
        if (!AppendString(Candidate, MAX_PATH_LENGTH, L"UdfsBench-") ||
            !AppendFormat(Candidate, MAX_PATH_LENGTH, L"%lu-", GetCurrentProcessId()) ||
            !AppendFormat(Candidate, MAX_PATH_LENGTH, L"%lu-", GetTickCount()) ||
            !AppendFormat(Candidate, MAX_PATH_LENGTH, L"%lu", Attempt))
            return FALSE;

        if (CreateDirectoryW(Candidate, NULL))
        {
            Length = (DWORD)wcslen(Candidate);
            if (Length >= RunPathCapacity)
            {
                RemoveDirectoryW(Candidate);
                return FALSE;
            }
            wcscpy(RunPath, Candidate);
            return TRUE;
        }

        if (GetLastError() != ERROR_ALREADY_EXISTS)
            return FALSE;
    }

    SetLastError(ERROR_ALREADY_EXISTS);
    return FALSE;
}

static BOOL
MakeWorkloadDirectory(PCWSTR RunPath, PCWSTR Workload, DWORD Repeat,
                      WCHAR *Directory, SIZE_T DirectoryCapacity)
{
    wcscpy(Directory, RunPath);
    if (!AppendString(Directory, DirectoryCapacity, L"\\") ||
        !AppendString(Directory, DirectoryCapacity, Workload) ||
        !AppendString(Directory, DirectoryCapacity, L"-r") ||
        !AppendFormat(Directory, DirectoryCapacity, L"%lu", Repeat))
        return FALSE;

    return CreateDirectoryW(Directory, NULL);
}

static BOOL
MakeFilePath(PCWSTR Directory, DWORD Index, WCHAR *Path, SIZE_T PathCapacity)
{
    wcscpy(Path, Directory);
    if (!AppendString(Path, PathCapacity, L"\\file-") ||
        !AppendFormat(Path, PathCapacity, L"%lu", Index) ||
        !AppendString(Path, PathCapacity, L".dat"))
        return FALSE;

    return TRUE;
}

static BOOL
SetArchiveMetadata(HANDLE File, BOOL TarLike, ULONGLONG Timestamp,
                   DWORD *Error)
{
    FILE_BASIC_INFORMATION BasicInfo;
    IO_STATUS_BLOCK IoStatus;
    NTSTATUS Status;

    ZeroMemory(&BasicInfo, sizeof(BasicInfo));
    BasicInfo.LastWriteTime.QuadPart = (LONGLONG)Timestamp;
    BasicInfo.FileAttributes = FILE_ATTRIBUTE_ARCHIVE;

    if (TarLike)
    {
        BasicInfo.CreationTime.QuadPart = (LONGLONG)(Timestamp - 20000000ULL);
        BasicInfo.LastAccessTime.QuadPart = (LONGLONG)(Timestamp - 10000000ULL);
    }

    Status = NtSetInformationFile(File, &IoStatus, &BasicInfo,
                                  sizeof(BasicInfo), FileBasicInformation);
    if (NT_SUCCESS(Status) && NT_SUCCESS(IoStatus.Status))
        return TRUE;

    *Error = RtlNtStatusToDosError(NT_SUCCESS(Status) ?
                                   IoStatus.Status : Status);
    return FALSE;
}

static void
RemoveWorkload(PCWSTR Directory, DWORD FileCount, BOOL KeepFiles)
{
    DWORD Index;
    WCHAR Path[MAX_PATH_LENGTH];

    if (KeepFiles)
        return;

    for (Index = 0; Index < FileCount; ++Index)
    {
        if (MakeFilePath(Directory, Index, Path, MAX_PATH_LENGTH))
            DeleteFileW(Path);
    }
    RemoveDirectoryW(Directory);
}

static BOOL
RunWorkload(PCWSTR RunPath, PCWSTR Name, BOOL TarLike,
            const BENCHMARK_CONFIG *Config, DWORD Repeat,
            const BYTE *Data, BENCHMARK_RESULT *Result)
{
    HANDLE *Files;
    WCHAR Directory[MAX_PATH_LENGTH];
    WCHAR Path[MAX_PATH_LENGTH];
    DWORD Index;
    DWORD BytesWritten;
    LARGE_INTEGER Start, End, Frequency;
    FILETIME CurrentTime;
    ULONGLONG MetadataBaseTime, Timestamp;
    BOOL Success = FALSE;
    DWORD Error = ERROR_SUCCESS;

    ZeroMemory(Result, sizeof(*Result));

    if (!MakeWorkloadDirectory(RunPath, Name, Repeat, Directory,
                               MAX_PATH_LENGTH))
    {
        fwprintf(stderr, L"Cannot create workload directory (error %lu).\n",
                 GetLastError());
        return FALSE;
    }

    Files = (HANDLE *)calloc(Config->FileCount, sizeof(HANDLE));
    if (!Files)
    {
        Error = ERROR_NOT_ENOUGH_MEMORY;
        goto Cleanup;
    }

    if (!QueryPerformanceFrequency(&Frequency) || Frequency.QuadPart == 0)
    {
        Error = GetLastError();
        goto Cleanup;
    }

    if (!QueryPerformanceCounter(&Start))
    {
        Error = GetLastError();
        goto Cleanup;
    }
    for (Index = 0; Index < Config->FileCount; ++Index)
    {
        if (!MakeFilePath(Directory, Index, Path, MAX_PATH_LENGTH))
        {
            Error = ERROR_FILENAME_EXCED_RANGE;
            goto Cleanup;
        }

        Files[Index] = CreateFileW(Path, GENERIC_WRITE | FILE_WRITE_ATTRIBUTES,
                                   0, NULL, CREATE_NEW, FILE_ATTRIBUTE_NORMAL,
                                   NULL);
        if (Files[Index] == INVALID_HANDLE_VALUE)
        {
            Files[Index] = NULL;
            Error = GetLastError();
            goto Cleanup;
        }
    }
    if (!QueryPerformanceCounter(&End))
    {
        Error = GetLastError();
        goto Cleanup;
    }
    Result->CreateTime.QuadPart = End.QuadPart - Start.QuadPart;

    if (!QueryPerformanceCounter(&Start))
    {
        Error = GetLastError();
        goto Cleanup;
    }
    for (Index = 0; Index < Config->FileCount; ++Index)
    {
        if (!WriteFile(Files[Index], Data, Config->FileSize, &BytesWritten, NULL) ||
            BytesWritten != Config->FileSize)
        {
            Error = GetLastError();
            if (Error == ERROR_SUCCESS)
                Error = ERROR_WRITE_FAULT;
            goto Cleanup;
        }
    }
    if (!QueryPerformanceCounter(&End))
    {
        Error = GetLastError();
        goto Cleanup;
    }
    Result->WriteTime.QuadPart = End.QuadPart - Start.QuadPart;

    GetSystemTimeAsFileTime(&CurrentTime);
    MetadataBaseTime = FileTimeToInteger(&CurrentTime);
    if (!QueryPerformanceCounter(&Start))
    {
        Error = GetLastError();
        goto Cleanup;
    }
    for (Index = 0; Index < Config->FileCount; ++Index)
    {
        Timestamp = MetadataBaseTime + ((ULONGLONG)Index * 10000000ULL);
        if (!SetArchiveMetadata(Files[Index], TarLike, Timestamp, &Error))
            goto Cleanup;
    }
    if (!QueryPerformanceCounter(&End))
    {
        Error = GetLastError();
        goto Cleanup;
    }
    Result->MetadataTime.QuadPart = End.QuadPart - Start.QuadPart;

    if (!QueryPerformanceCounter(&Start))
    {
        Error = GetLastError();
        goto Cleanup;
    }
    for (Index = 0; Index < Config->FileCount; ++Index)
    {
        if (!FlushFileBuffers(Files[Index]))
        {
            Error = GetLastError();
            goto Cleanup;
        }
    }
    if (!QueryPerformanceCounter(&End))
    {
        Error = GetLastError();
        goto Cleanup;
    }
    Result->FlushTime.QuadPart = End.QuadPart - Start.QuadPart;

    if (!QueryPerformanceCounter(&Start))
    {
        Error = GetLastError();
        goto Cleanup;
    }
    for (Index = 0; Index < Config->FileCount; ++Index)
    {
        if (!CloseHandle(Files[Index]))
        {
            Error = GetLastError();
            goto Cleanup;
        }
        Files[Index] = NULL;
    }
    if (!QueryPerformanceCounter(&End))
    {
        Error = GetLastError();
        goto Cleanup;
    }
    Result->CloseTime.QuadPart = End.QuadPart - Start.QuadPart;
    Success = TRUE;

Cleanup:
    if (Files)
    {
        for (Index = 0; Index < Config->FileCount; ++Index)
        {
            if (Files[Index])
                CloseHandle(Files[Index]);
        }
        free(Files);
    }

    RemoveWorkload(Directory, Config->FileCount, Config->KeepFiles);

    if (!Success)
    {
        fwprintf(stderr, L"%ls workload failed (error %lu).\n", Name, Error);
        SetLastError(Error);
    }
    else
    {
        double WriteMs = Milliseconds(Result->WriteTime.QuadPart,
                                      Frequency.QuadPart);
        double MetadataMs = Milliseconds(Result->MetadataTime.QuadPart,
                                         Frequency.QuadPart);
        double FlushMs = Milliseconds(Result->FlushTime.QuadPart,
                                      Frequency.QuadPart);
        double CloseMs = Milliseconds(Result->CloseTime.QuadPart,
                                      Frequency.QuadPart);
        double MiB = (double)Config->FileCount * Config->FileSize /
                     (1024.0 * 1024.0);

        double CreateMs = Milliseconds(Result->CreateTime.QuadPart,
                                       Frequency.QuadPart);

        wprintf(L"%ls repeat %lu: create %.3f ms, write %.3f ms "
                L"(%.2f MiB/s), metadata %.3f ms (%.2f us/file), "
                L"flush %.3f ms, close %.3f ms\n",
                Name, Repeat + 1, CreateMs, WriteMs,
                WriteMs ? MiB * 1000.0 / WriteMs : 0.0,
                MetadataMs,
                MetadataMs * 1000.0 / Config->FileCount,
                FlushMs, CloseMs);
    }

    return Success;
}

static void
PrintUsage(PCWSTR Program)
{
    fwprintf(stderr,
             L"Usage: %ls <existing-directory> [files=100] [bytes-per-file=4096] "
             L"[repeats=3] [--keep]\n"
             L"Creates temporary benchmark files under the selected directory.\n",
             Program);
}

int
wmain(int argc, WCHAR *argv[])
{
    BENCHMARK_CONFIG Config;
    BENCHMARK_RESULT Result;
    WCHAR RunPath[MAX_PATH_LENGTH];
    BYTE *Data;
    DWORD Argument = 2;
    DWORD Repeat;
    BOOL Success = TRUE;
    ULONGLONG TotalBytes;

    Config.FileCount = DEFAULT_FILE_COUNT;
    Config.FileSize = DEFAULT_FILE_SIZE;
    Config.Repeats = DEFAULT_REPEATS;
    Config.KeepFiles = FALSE;

    if (argc < 2)
    {
        PrintUsage(argv[0]);
        return 2;
    }

    if (Argument < (DWORD)argc && argv[Argument][0] != L'-')
    {
        if (!ParseNumber(argv[Argument++], 1, MAX_FILE_COUNT,
                         &Config.FileCount))
        {
            PrintUsage(argv[0]);
            return 2;
        }
    }
    if (Argument < (DWORD)argc && argv[Argument][0] != L'-')
    {
        if (!ParseNumber(argv[Argument++], 1, MAX_FILE_SIZE,
                         &Config.FileSize))
        {
            PrintUsage(argv[0]);
            return 2;
        }
    }
    if (Argument < (DWORD)argc && argv[Argument][0] != L'-')
    {
        if (!ParseNumber(argv[Argument++], 1, 100, &Config.Repeats))
        {
            PrintUsage(argv[0]);
            return 2;
        }
    }
    if (Argument < (DWORD)argc && wcscmp(argv[Argument], L"--keep") == 0)
    {
        Config.KeepFiles = TRUE;
        ++Argument;
    }
    if (Argument != (DWORD)argc)
    {
        PrintUsage(argv[0]);
        return 2;
    }

    TotalBytes = (ULONGLONG)Config.FileCount * Config.FileSize *
                 Config.Repeats * 2;
    if (TotalBytes > MAX_TOTAL_BYTES)
    {
        fwprintf(stderr,
                 L"Requested workload exceeds the 256 MiB total data limit.\n");
        return 2;
    }

    Data = (BYTE *)malloc(Config.FileSize);
    if (!Data)
    {
        fwprintf(stderr, L"Not enough memory for the write buffer.\n");
        return 1;
    }
    {
        DWORD i;
        for (i = 0; i < Config.FileSize; ++i)
            Data[i] = (BYTE)(i % 251);
    }

    if (!CreateRunDirectory(argv[1], RunPath, MAX_PATH_LENGTH))
    {
        fwprintf(stderr, L"Cannot create benchmark directory (error %lu).\n",
                 GetLastError());
        free(Data);
        return 1;
    }

    wprintf(L"Benchmark files: %lu; bytes/file: %lu; repeats: %lu\n"
            L"Output directory: %ls\n"
            L"Metadata operation: one NtSetInformationFile(FileBasicInformation) "
            L"per file; TAR-like sets creation/access/write times and archive "
            L"attribute, ZIP-like sets write time and archive attribute.\n",
            Config.FileCount, Config.FileSize, Config.Repeats, RunPath);
    wprintf(L"Creation, data writing, metadata, flush, and close are timed "
            L"separately. Flush is an explicit "
            L"FlushFileBuffers per file; it is not the normal 7-Zip close path.\n");

    for (Repeat = 0; Repeat < Config.Repeats; ++Repeat)
    {
        BOOL RunTarFirst = (Repeat % 2) == 0;
        if (RunTarFirst)
        {
            Success = RunWorkload(RunPath, L"tar-like", TRUE, &Config,
                                  Repeat, Data, &Result);
            if (Success)
                Success = RunWorkload(RunPath, L"zip-like", FALSE, &Config,
                                      Repeat, Data, &Result);
        }
        else
        {
            Success = RunWorkload(RunPath, L"zip-like", FALSE, &Config,
                                  Repeat, Data, &Result);
            if (Success)
                Success = RunWorkload(RunPath, L"tar-like", TRUE, &Config,
                                      Repeat, Data, &Result);
        }

        if (!Success)
            break;
    }

    free(Data);

    if (Config.KeepFiles)
    {
        wprintf(L"Kept benchmark output at %ls\n", RunPath);
    }
    else
    {
        RemoveDirectoryW(RunPath);
    }

    return Success ? 0 : 1;
}
