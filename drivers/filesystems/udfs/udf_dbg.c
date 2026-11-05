////////////////////////////////////////////////////////////////////
// Copyright (C) Alexander Telyatnikov, Ivan Keliukh, Yegor Anchishkin, SKIF Software, 1999-2013. Kiev, Ukraine
// All rights reserved
// This file was released under the GPLv2 on June 2015.
////////////////////////////////////////////////////////////////////
#include "udffs.h"
#if defined(UDF_DBG)

//#define TRACK_REF_COUNTERS

ULONG UdfTimeStamp = -1;

#define MAX_MEM_DEBUG_DESCRIPTORS 8192

typedef struct _MEM_DESC {
    ULONG   Length;
    PCHAR   Addr;
#ifdef TRACK_SYS_ALLOC_CALLERS
    ULONG   SrcId;
    ULONG   SrcLine;
#endif //TRACK_SYS_ALLOC_CALLERS
    POOL_TYPE Type;
} MEM_DESC, *PMEM_DESC;


MEM_DESC    MemDesc[MAX_MEM_DEBUG_DESCRIPTORS];
ULONG       cur_max = 0;
ULONG       AllocCountPaged = 0;
ULONG       AllocCountNPaged = 0;
ULONG       MemDescInited = 0;

PVOID
DebugAllocatePool(
   POOL_TYPE Type,
   ULONG size
#ifdef TRACK_SYS_ALLOC_CALLERS
 , ULONG SrcId,
   ULONG SrcLine
#endif //TRACK_SYS_ALLOC_CALLERS
) {
    ULONG i;
//    UDFPrint(("SysAllocated: %x\n",AllocCount));
    if (!MemDescInited) {
        RtlZeroMemory(&MemDesc, sizeof(MemDesc));
        MemDescInited = 1;
    }
    for (i=0;i<cur_max;i++) {
        if (MemDesc[i].Addr==NULL) {
            MemDesc[i].Addr = (PCHAR)ExAllocatePoolWithTag(Type, (size), 'Fnwd'); // dwnF

            ASSERT(MemDesc[i].Addr);

            if (MemDesc[i].Addr) {
                if (Type == PagedPool) {
                    AllocCountPaged += (size+7) & ~7;
                } else {
                    AllocCountNPaged += (size+7) & ~7;
                }
            }

            MemDesc[i].Length = size;
            MemDesc[i].Type = Type;
#ifdef TRACK_SYS_ALLOC_CALLERS
            MemDesc[i].SrcId   = SrcId;
            MemDesc[i].SrcLine = SrcLine;
#endif //TRACK_SYS_ALLOC_CALLERS
            return MemDesc[i].Addr;
        }
    }
    if (cur_max == MAX_MEM_DEBUG_DESCRIPTORS) {
        UDFPrint(("Debug memory descriptor list full\n"));
        return ExAllocatePoolWithTag(Type, (size) , 'Fnwd');
    }

    MemDesc[i].Addr = (PCHAR)ExAllocatePoolWithTag(Type, (size) , 'Fnwd');

    if (MemDesc[i].Addr) {
        if (Type == PagedPool) {
            AllocCountPaged += (size+7) & ~7;
        } else {
            AllocCountNPaged += (size+7) & ~7;
        }
    }

    MemDesc[i].Length = (size);
#ifdef TRACK_SYS_ALLOC_CALLERS
    MemDesc[i].SrcId   = SrcId;
    MemDesc[i].SrcLine = SrcLine;
#endif //TRACK_SYS_ALLOC_CALLERS
    MemDesc[i].Type = Type;
    cur_max++;
    return MemDesc[cur_max-1].Addr;

}

VOID DebugFreePool(PVOID addr) {
    ULONG i;

    ASSERT(addr);

    for (i=0;i<cur_max;i++) {
        if (MemDesc[i].Addr == addr)  {

            if (MemDesc[i].Type == PagedPool) {
                AllocCountPaged -= (MemDesc[i].Length+7) & ~7;
            } else {
                AllocCountNPaged -= (MemDesc[i].Length+7) & ~7;
            }

            MemDesc[i].Addr = NULL;
            MemDesc[i].Length = 0;
#ifdef TRACK_SYS_ALLOC_CALLERS
            MemDesc[i].SrcId   = 0;
            MemDesc[i].SrcLine = 0;
#endif //TRACK_SYS_ALLOC_CALLERS
            goto not_bug;
        }
    }
    if (i==cur_max && cur_max != MAX_MEM_DEBUG_DESCRIPTORS) {
        UDFPrint(("Buug! - Deallocating nonallocated block\n"));
        return;
    }
not_bug:
//    UDFPrint(("SysAllocated: %x\n",AllocCount));
    ExFreePool(addr);
}

#endif // UDF_DBG

#if defined(UDF_FUNCTION_TIMING)

ULONG
_cdecl
DbgPrint(
    const char *Format,
    ...
    );

#define UDF_TIMING_THREAD_COUNT 64
#define UDF_TIMING_MAX_DEPTH 64
#define UDF_TIMING_NO_INSTRUMENT __attribute__((no_instrument_function))

typedef struct _UDF_FUNCTION_TIMING_FRAME {
    PVOID Function;
    LARGE_INTEGER Start;
    LARGE_INTEGER Frequency;
} UDF_FUNCTION_TIMING_FRAME;

typedef struct _UDF_FUNCTION_TIMING_THREAD {
    PETHREAD Thread;
    ULONG Depth;
    ULONG OverflowDepth;
    UDF_FUNCTION_TIMING_FRAME Frames[UDF_TIMING_MAX_DEPTH];
} UDF_FUNCTION_TIMING_THREAD;

static KSPIN_LOCK UdfFunctionTimingLock;
static volatile LONG UdfFunctionTimingInitialization;
static UDF_FUNCTION_TIMING_THREAD UdfFunctionTimingThreads[UDF_TIMING_THREAD_COUNT];

void UDF_TIMING_NO_INSTRUMENT
__cyg_profile_func_enter(PVOID Function, PVOID CallSite)
{
    PETHREAD Thread = PsGetCurrentThread();
    LARGE_INTEGER Frequency;
    LARGE_INTEGER Start = KeQueryPerformanceCounter(&Frequency);
    KIRQL OldIrql;
    ULONG i;
    UDF_FUNCTION_TIMING_THREAD *TimingThread = NULL;

    UNREFERENCED_PARAMETER(CallSite);

    if (InterlockedCompareExchange(&UdfFunctionTimingInitialization, 1, 0) == 0) {
        KeInitializeSpinLock(&UdfFunctionTimingLock);
        InterlockedExchange(&UdfFunctionTimingInitialization, 2);
    } else {
        while (InterlockedCompareExchange(&UdfFunctionTimingInitialization, 2, 2) != 2) {
            YieldProcessor();
        }
    }

    KeAcquireSpinLock(&UdfFunctionTimingLock, &OldIrql);
    for (i = 0; i < UDF_TIMING_THREAD_COUNT; i++) {
        if (UdfFunctionTimingThreads[i].Thread == Thread) {
            TimingThread = &UdfFunctionTimingThreads[i];
            break;
        }
        if (!TimingThread && !UdfFunctionTimingThreads[i].Thread) {
            TimingThread = &UdfFunctionTimingThreads[i];
        }
    }

    if (TimingThread) {
        if (!TimingThread->Thread) {
            TimingThread->Thread = Thread;
            TimingThread->Depth = 0;
            TimingThread->OverflowDepth = 0;
        }
        if (TimingThread->OverflowDepth) {
            TimingThread->OverflowDepth++;
        } else if (TimingThread->Depth == UDF_TIMING_MAX_DEPTH) {
            TimingThread->OverflowDepth = 1;
        } else {
            UDF_FUNCTION_TIMING_FRAME *Frame = &TimingThread->Frames[TimingThread->Depth++];
            Frame->Function = Function;
            Frame->Start = Start;
            Frame->Frequency = Frequency;
        }
    }
    KeReleaseSpinLock(&UdfFunctionTimingLock, OldIrql);
}

void UDF_TIMING_NO_INSTRUMENT
__cyg_profile_func_exit(PVOID Function, PVOID CallSite)
{
    PETHREAD Thread = PsGetCurrentThread();
    KIRQL OldIrql;
    ULONG i;
    UDF_FUNCTION_TIMING_THREAD *TimingThread = NULL;
    UDF_FUNCTION_TIMING_FRAME Frame;
    BOOLEAN PrintTiming = FALSE;

    UNREFERENCED_PARAMETER(CallSite);

    if (InterlockedCompareExchange(&UdfFunctionTimingInitialization, 2, 2) != 2)
        return;

    KeAcquireSpinLock(&UdfFunctionTimingLock, &OldIrql);
    for (i = 0; i < UDF_TIMING_THREAD_COUNT; i++) {
        if (UdfFunctionTimingThreads[i].Thread == Thread) {
            TimingThread = &UdfFunctionTimingThreads[i];
            break;
        }
    }

    if (TimingThread) {
        if (TimingThread->OverflowDepth) {
            TimingThread->OverflowDepth--;
        } else if (TimingThread->Depth &&
                   TimingThread->Frames[TimingThread->Depth - 1].Function == Function) {
            Frame = TimingThread->Frames[--TimingThread->Depth];
            PrintTiming = TRUE;
        }
        if (!TimingThread->Depth && !TimingThread->OverflowDepth)
            TimingThread->Thread = NULL;
    }
    KeReleaseSpinLock(&UdfFunctionTimingLock, OldIrql);

    if (PrintTiming) {
        LARGE_INTEGER End = KeQueryPerformanceCounter(NULL);
        ULONGLONG ElapsedUs = 0;
        if (Frame.Frequency.QuadPart > 0 && End.QuadPart >= Frame.Start.QuadPart) {
            ULONGLONG Ticks = End.QuadPart - Frame.Start.QuadPart;
            ULONGLONG Frequency = Frame.Frequency.QuadPart;
            ElapsedUs = (Ticks / Frequency) * 1000000 +
                        ((Ticks % Frequency) * 1000000) / Frequency;
        }
        DbgPrint("UDF timing: %p took %I64u us\n", Function, ElapsedUs);
    }
}

#endif // UDF_FUNCTION_TIMING
