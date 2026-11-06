#include "udffs.h"

#if defined(UDF_FUNCTION_TIMING)

#define UDF_TIMING_THREAD_COUNT 64
#define UDF_TIMING_MAX_DEPTH 16

ULONG
_cdecl DbgPrint(const char *Format, ...);

#if defined(_MSC_VER)
#include <intrin.h>
#endif

static PETHREAD
UdfFunctionTimingCurrentThread(VOID)
{
#if defined(_MSC_VER) && defined(_M_IX86)
    return (PETHREAD)__readfsdword(0x124);
#elif defined(_MSC_VER) && defined(_M_AMD64)
    return (PETHREAD)__readgsqword(0x188);
#elif defined(_MSC_VER)
    return (PETHREAD)KeGetCurrentThread();
#else
    return PsGetCurrentThread();
#endif
}

typedef struct _UDF_FUNCTION_TIMING_FRAME
{
    PVOID Function;
    LARGE_INTEGER Start;
} UDF_FUNCTION_TIMING_FRAME;

typedef struct _UDF_FUNCTION_TIMING_THREAD
{
    PETHREAD Thread;
    ULONG Depth;
    ULONG OverflowDepth;
    UDF_FUNCTION_TIMING_FRAME Frames[UDF_TIMING_MAX_DEPTH];
} UDF_FUNCTION_TIMING_THREAD;

static KSPIN_LOCK UdfFunctionTimingLock = 0;
static UDF_FUNCTION_TIMING_THREAD UdfFunctionTimingThreads[UDF_TIMING_THREAD_COUNT];

static void
UdfFunctionTimingEnter(PVOID Function, LARGE_INTEGER Start)
{
    PETHREAD Thread = UdfFunctionTimingCurrentThread();
    KIRQL OldIrql;
    ULONG i;
    UDF_FUNCTION_TIMING_THREAD *TimingThread = NULL;

    KeAcquireSpinLock(&UdfFunctionTimingLock, &OldIrql);
    for (i = 0; i < UDF_TIMING_THREAD_COUNT; i++)
    {
        if (UdfFunctionTimingThreads[i].Thread == Thread)
        {
            TimingThread = &UdfFunctionTimingThreads[i];
            break;
        }
        if (!TimingThread && !UdfFunctionTimingThreads[i].Thread)
            TimingThread = &UdfFunctionTimingThreads[i];
    }

    if (TimingThread)
    {
        if (!TimingThread->Thread)
        {
            TimingThread->Thread = Thread;
            TimingThread->Depth = 0;
            TimingThread->OverflowDepth = 0;
        }
        if (TimingThread->OverflowDepth)
        {
            TimingThread->OverflowDepth++;
        }
        else if (TimingThread->Depth == UDF_TIMING_MAX_DEPTH)
        {
            TimingThread->OverflowDepth = 1;
        }
        else
        {
            UDF_FUNCTION_TIMING_FRAME *Frame = &TimingThread->Frames[TimingThread->Depth++];
            Frame->Function = Function;
            Frame->Start = Start;
        }
    }
    KeReleaseSpinLock(&UdfFunctionTimingLock, OldIrql);
}

static BOOLEAN
UdfFunctionTimingExit(UDF_FUNCTION_TIMING_FRAME *Frame)
{
    PETHREAD Thread = UdfFunctionTimingCurrentThread();
    KIRQL OldIrql;
    ULONG i;
    UDF_FUNCTION_TIMING_THREAD *TimingThread = NULL;
    BOOLEAN FrameFound = FALSE;

    KeAcquireSpinLock(&UdfFunctionTimingLock, &OldIrql);
    for (i = 0; i < UDF_TIMING_THREAD_COUNT; i++)
    {
        if (UdfFunctionTimingThreads[i].Thread == Thread)
        {
            TimingThread = &UdfFunctionTimingThreads[i];
            break;
        }
    }

    if (TimingThread)
    {
        if (TimingThread->OverflowDepth)
        {
            TimingThread->OverflowDepth--;
        }
        else if (TimingThread->Depth)
        {
            *Frame = TimingThread->Frames[--TimingThread->Depth];
            FrameFound = TRUE;
        }
        if (!TimingThread->Depth && !TimingThread->OverflowDepth)
            TimingThread->Thread = NULL;
    }
    KeReleaseSpinLock(&UdfFunctionTimingLock, OldIrql);
    return FrameFound;
}

static void
UdfFunctionTimingPrint(UDF_FUNCTION_TIMING_FRAME *Frame)
{
    LARGE_INTEGER Frequency;
    LARGE_INTEGER End = KeQueryPerformanceCounter(&Frequency);
    ULONGLONG ElapsedUs = 0;

    if (Frequency.QuadPart > 0 && End.QuadPart >= Frame->Start.QuadPart)
    {
        ULONGLONG Ticks = End.QuadPart - Frame->Start.QuadPart;
        ULONGLONG FrequencyValue = Frequency.QuadPart;
        ElapsedUs = (Ticks / FrequencyValue) * 1000000 + ((Ticks % FrequencyValue) * 1000000) / FrequencyValue;
    }
    DbgPrint("UDF timing: %p took %I64u us\n", Frame->Function, ElapsedUs);
}

#if defined(_MSC_VER) && !defined(__clang__)

void __cdecl _penter(void)
{
    LARGE_INTEGER Start = KeQueryPerformanceCounter(NULL);
    UdfFunctionTimingEnter(_ReturnAddress(), Start);
}

void __cdecl _pexit(void)
{
    UDF_FUNCTION_TIMING_FRAME Frame;
    if (UdfFunctionTimingExit(&Frame))
        UdfFunctionTimingPrint(&Frame);
}

#else

#define UDF_TIMING_NO_INSTRUMENT __attribute__((no_instrument_function))

void UDF_TIMING_NO_INSTRUMENT
__cyg_profile_func_enter(PVOID Function, PVOID CallSite)
{
    LARGE_INTEGER Start = KeQueryPerformanceCounter(NULL);
    UNREFERENCED_PARAMETER(CallSite);
    UdfFunctionTimingEnter(Function, Start);
}

void UDF_TIMING_NO_INSTRUMENT
__cyg_profile_func_exit(PVOID Function, PVOID CallSite)
{
    UDF_FUNCTION_TIMING_FRAME Frame;
    UNREFERENCED_PARAMETER(Function);
    UNREFERENCED_PARAMETER(CallSite);
    if (UdfFunctionTimingExit(&Frame))
        UdfFunctionTimingPrint(&Frame);
}

#endif

#endif
