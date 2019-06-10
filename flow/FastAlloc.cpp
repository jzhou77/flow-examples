/*
 * FastAlloc.cpp
 *
 * This source file is part of the FoundationDB open source project
 *
 * Copyright 2013-2018 Apple Inc. and the FoundationDB project authors
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "flow/FastAlloc.h"

#include "flow/ThreadPrimitives.h"
#include "flow/Trace.h"
#include "flow/Error.h"
#include "flow/Knobs.h"
#include "flow/flow.h"

#include <cstdint>
#include <unordered_map>

#ifdef WIN32
#include <windows.h>
#undef min
#undef max
#endif

#ifdef __linux__
#include <sys/mman.h>
#include <linux/mman.h>
#endif

#define FAST_ALLOCATOR_DEBUG 0

#ifdef _MSC_VER
// warning 4073 warns about "initializers put in library initialization area", which is our intent
#pragma warning (disable: 4073)
#pragma init_seg(lib)
#define INIT_SEG
#elif defined(__GNUG__)
#ifdef __linux__
#define INIT_SEG __attribute__ ((init_priority (1000)))
#elif defined(__APPLE__)
#pragma message "init_priority is not supported on this platform; will this be a problem?"
#define INIT_SEG
#else
#error Where am I?
#endif
#else
#error Port me? (init_seg(lib))
#endif

template<int Size>
INIT_SEG thread_local typename FastAllocator<Size>::ThreadData FastAllocator<Size>::threadData;

template<int Size>
thread_local bool FastAllocator<Size>::threadInitialized = false;

#ifdef VALGRIND
template<int Size>
unsigned long FastAllocator<Size>::vLock = 1;
#endif

template<int Size>
void* FastAllocator<Size>::freelist = nullptr;

typedef void (*ThreadInitFunction)();

ThreadInitFunction threadInitFunction = 0;  // See ThreadCleanup.cpp in the C binding
void setFastAllocatorThreadInitFunction( ThreadInitFunction f ) { 
	ASSERT( !threadInitFunction );
	threadInitFunction = f; 
}

int64_t g_hugeArenaMemory = 0;

double hugeArenaLastLogged = 0;
std::map<std::string, std::pair<int,int>> hugeArenaTraces;

void hugeArenaSample(int size) {
	auto& info = hugeArenaTraces[platform::get_backtrace()];
	info.first++;
	info.second+=size;
	if(now() - hugeArenaLastLogged > FLOW_KNOBS->HUGE_ARENA_LOGGING_INTERVAL) {
		for(auto& it : hugeArenaTraces) {
			TraceEvent("HugeArenaSample").detail("Count", it.second.first).detail("Size", it.second.second).detail("Backtrace", it.first);
		}
		hugeArenaLastLogged = now();
		hugeArenaTraces.clear();
	}
}

#ifdef ALLOC_INSTRUMENTATION
INIT_SEG std::map<const char*, AllocInstrInfo> allocInstr;
INIT_SEG std::unordered_map<int64_t, std::pair<uint32_t, size_t>> memSample;
INIT_SEG std::unordered_map<uint32_t, BackTraceAccount> backTraceLookup;
INIT_SEG ThreadSpinLock memLock;
const size_t SAMPLE_BYTES = 1e7;
template<int Size>
volatile int32_t FastAllocator<Size>::pageCount;
thread_local bool memSample_entered = false;
#endif

#ifdef ALLOC_INSTRUMENTATION_STDOUT
thread_local bool inRecordAllocation = false;
#endif

void recordAllocation( void *ptr, size_t size ) {
#ifdef ALLOC_INSTRUMENTATION_STDOUT
	if( inRecordAllocation )
		return;
	inRecordAllocation = true;
	std::string trace = platform::get_backtrace();
	printf("Alloc\t%p\t%d\t%s\n", ptr, size, trace.c_str());
	inRecordAllocation = false;
#endif
#ifdef ALLOC_INSTRUMENTATION
	if( memSample_entered )
		return;
	memSample_entered = true;

	if(((double)rand()) / RAND_MAX < ((double)size) / SAMPLE_BYTES) {
		void *buffer[100];
#if defined(__linux__)
		int nptrs = backtrace( buffer, 100 );
#elif defined(_WIN32)
		// We could be using fourth parameter to get a hash, but we'll do this
		//  in a unified way between platforms
		int nptrs = CaptureStackBackTrace( 1, 100, buffer, NULL );
#else
#error Instrumentation not supported on this platform
#endif

		uint32_t a = 0, b = 0;
		if( nptrs > 0 ) {
			hashlittle2( buffer, nptrs * sizeof(void *), &a, &b );
		}

		double countDelta = std::max(1.0, ((double)SAMPLE_BYTES) / size);
		size_t sizeDelta = std::max(SAMPLE_BYTES, size);
		ThreadSpinLockHolder holder( memLock );
		auto it = backTraceLookup.find( a );
		if( it == backTraceLookup.end() ) {
			auto& bt = backTraceLookup[ a ];
			bt.backTrace = new std::vector<void*>();
			for (int j = 0; j < nptrs; j++) {
				bt.backTrace->push_back( buffer[j] );
			}
			bt.totalSize = sizeDelta;
			bt.count = countDelta;
			bt.sampleCount = 1;
		} else {
			it->second.totalSize += sizeDelta;
			it->second.count += countDelta;
			it->second.sampleCount++;
		}
		memSample[(int64_t)ptr] = std::make_pair(a, size);
	}
	memSample_entered = false;
#endif
}

void recordDeallocation( void *ptr ) {
#ifdef ALLOC_INSTRUMENTATION_STDOUT
	if( inRecordAllocation )
		return;
	printf("Dealloc\t%p\n", ptr);
	inRecordAllocation = false;
#endif
#ifdef ALLOC_INSTRUMENTATION
	if( memSample_entered ) // could this lead to deallocations not being recorded?
		return;
	memSample_entered = true;
	{
		ThreadSpinLockHolder holder( memLock );

		auto it = memSample.find( (int64_t)ptr );
		if( it == memSample.end() ) {
			memSample_entered = false;
			return;
		}
		auto bti = backTraceLookup.find( it->second.first );
		ASSERT( bti != backTraceLookup.end() );

		size_t sizeDelta = std::max(SAMPLE_BYTES, it->second.second);
		double countDelta = std::max(1.0, ((double)SAMPLE_BYTES) / it->second.second);

		bti->second.totalSize -= sizeDelta;
		bti->second.count -= countDelta;
		bti->second.sampleCount--;
		memSample.erase( it );
	}
	memSample_entered = false;
#endif
}

template <int Size>
struct FastAllocator<Size>::GlobalData {
	CRITICAL_SECTION mutex;
	std::vector<void*> magazines;   // These magazines are always exactly magazine_size ("full")
	std::vector<std::pair<int, void*>> partial_magazines;  // Magazines that are not "full" and their counts.  Only created by releaseThreadMagazines().
	long long totalMemory;
	long long partialMagazineUnallocatedMemory;
	long long activeThreads;
	GlobalData() : totalMemory(0), partialMagazineUnallocatedMemory(0), activeThreads(0) { 
		InitializeCriticalSection(&mutex);
	}
};

template <int Size>
long long FastAllocator<Size>::getTotalMemory() {
	return globalData()->totalMemory;
}

// This does not include memory held by various threads that's available for allocation
template <int Size>
long long FastAllocator<Size>::getApproximateMemoryUnused() {
	return globalData()->magazines.size() * magazine_size * Size + globalData()->partialMagazineUnallocatedMemory;
}

template <int Size>
long long FastAllocator<Size>::getActiveThreads() {
	return globalData()->activeThreads;
}

static int64_t getSizeCode(int i) {
	switch (i) {
		case 16: return 1;
		case 32: return 2;
		case 64: return 3;
		case 128: return 4;
		case 256: return 5;
		case 512: return 6;
		case 1024: return 7;
		case 2048: return 8;
		case 4096: return 9;
		case 8192: return 10;
		default: return 11;
	}
}

template<int Size>
void *FastAllocator<Size>::allocate() {
	if(!threadInitialized) {
		initThread();
	}

#if FASTALLOC_THREAD_SAFE
	ThreadData& thr = threadData;
	if (!thr.freelist) {
		ASSERT(thr.count == 0);
		if (thr.alternate) {
			thr.freelist = thr.alternate;
			thr.alternate = nullptr;
			thr.count = magazine_size;
		} else {
			getMagazine();
		}
	}
	--thr.count;
	void* p = thr.freelist;
#if VALGRIND
	VALGRIND_MAKE_MEM_DEFINED(p, sizeof(void*));
#endif
	thr.freelist = *(void**)p;
	ASSERT(!thr.freelist == (thr.count == 0)); // freelist is empty if and only if count is 0
	//check( p, true );
#else
	void* p = freelist;
	if (!p) getMagazine();
#if VALGRIND
	VALGRIND_MAKE_MEM_DEFINED(p, sizeof(void*));
#endif
	freelist = *(void**)p;
#endif
#if VALGRIND
	VALGRIND_MALLOCLIKE_BLOCK( p, Size, 0, 0 );
#endif
#if defined(ALLOC_INSTRUMENTATION) || defined(ALLOC_INSTRUMENTATION_STDOUT)
	recordAllocation(p, Size);
#endif
	return p;
}

template<int Size>
void FastAllocator<Size>::release(void *ptr) {
	if(!threadInitialized) {
		initThread();
	}

#if FASTALLOC_THREAD_SAFE
	ThreadData& thr = threadData;
	if (thr.count == magazine_size) {
		if (thr.alternate)		// Two full magazines, return one
			releaseMagazine( thr.alternate );
		thr.alternate = thr.freelist;
		thr.freelist = nullptr;
		thr.count = 0;
	}

	ASSERT(!thr.freelist == (thr.count == 0)); // freelist is empty if and only if count is 0

	++thr.count;
	*(void**)ptr = thr.freelist;
	//check(ptr, false);
	thr.freelist = ptr;
#else
	*(void**)ptr = freelist;
	freelist = ptr;
#endif

#if VALGRIND
	VALGRIND_FREELIKE_BLOCK( ptr, 0 );
#endif
#if defined(ALLOC_INSTRUMENTATION) || defined(ALLOC_INSTRUMENTATION_STDOUT)
	recordDeallocation( ptr );
#endif
}

template <int Size>
void FastAllocator<Size>::check(void* ptr, bool alloc) {
#if FAST_ALLOCATOR_DEBUG
	//if (ptr == (void*)0x400200180)
	//	printf("%c%p\n", alloc?'+':'-', ptr);

	// Check for pointers that aren't part of this FastAllocator
	if (ptr < (void*)(((getSizeCode(Size)<<11) + 0) * magazine_size*Size) ||
		ptr > (void*)(((getSizeCode(Size)<<11) + 4000) * magazine_size*Size) ||
		(int64_t(ptr)&(Size-1)))
	{
		printf("Bad ptr: %p\n", ptr);
		abort();
	}

	// Redundant freelist pointers to detect outright smashing of the freelist
	if (alloc) {
		if ( *((void**)ptr+1) != *(void**)ptr ) {
			printf("Freelist corruption? %p %p\n", *(void**)ptr, *((void**)ptr+1));
			abort();
		}
		*((void**)ptr+1) = (void*)0;
	} else {
		*((void**)ptr+1) = *(void**)ptr;
	}

	// Track allocated/free status in a completely separate data structure to detect double frees
	int i = (int)((int64_t)ptr - ((getSizeCode(Size)<<11) + 0) * magazine_size*Size) / Size;
	static std::vector<bool> isFreed;
	if (!alloc) {
		if (i+1 > isFreed.size())
			isFreed.resize(i+1, false);
		if (isFreed[i]) {
			printf("Double free: %p\n", ptr);
			abort();
		}
		isFreed[i] = true;
	} else {
		if (i+1 > isFreed.size()) {
			printf("Allocate beyond end: %p\n", ptr);
			abort();
		}
		if (!isFreed[i]) {
			printf("Allocate non-freed: %p\n", ptr);
			abort();
		}
		isFreed[i] = false;
	}
#endif
}

template <int Size>
void FastAllocator<Size>::initThread() {
	threadInitialized = true;
	if (threadInitFunction) {
		threadInitFunction();
	}

	EnterCriticalSection(&globalData()->mutex);
	++globalData()->activeThreads;
	LeaveCriticalSection(&globalData()->mutex);

	threadData.freelist = nullptr;
	threadData.alternate = nullptr;
	threadData.count = 0;
}

template <int Size>
void FastAllocator<Size>::getMagazine() {
	ASSERT(threadInitialized);
	ASSERT(!threadData.freelist && !threadData.alternate && threadData.count == 0);

	EnterCriticalSection(&globalData()->mutex);
	if (globalData()->magazines.size()) {
		void* m = globalData()->magazines.back();
		globalData()->magazines.pop_back();
		LeaveCriticalSection(&globalData()->mutex);
		threadData.freelist = m;
		threadData.count = magazine_size;
		return;
	} else if (globalData()->partial_magazines.size()) {
		std::pair<int, void*> p = globalData()->partial_magazines.back();
		globalData()->partial_magazines.pop_back();
		globalData()->partialMagazineUnallocatedMemory -= p.first * Size;
		LeaveCriticalSection(&globalData()->mutex);
		threadData.freelist = p.second;
		threadData.count = p.first;
		return;
	}
	globalData()->totalMemory += magazine_size*Size;
	LeaveCriticalSection(&globalData()->mutex);

	// Allocate a new page of data from the system allocator
	#ifdef ALLOC_INSTRUMENTATION
	interlockedIncrement(&pageCount);
	#endif

	void** block = nullptr;
#if FAST_ALLOCATOR_DEBUG
#ifdef WIN32
	static int alt = 0; alt++;
	block = (void**)VirtualAllocEx( GetCurrentProcess(), 
									(void*)( ((getSizeCode(Size)<<11) + alt) * magazine_size*Size), magazine_size*Size, MEM_COMMIT|MEM_RESERVE, PAGE_READWRITE );
#else
	static int alt = 0; alt++;
	void* desiredBlock = (void*)( ((getSizeCode(Size)<<11) + alt) * magazine_size*Size);
	block = (void**)mmap( desiredBlock, magazine_size*Size, PROT_READ|PROT_WRITE, MAP_PRIVATE|MAP_ANONYMOUS, -1, 0 );
	ASSERT( block == desiredBlock );
#endif
#else
	// FIXME: We should be able to allocate larger magazine sizes here if we
	// detect that the underlying system supports hugepages.  Using hugepages
	// with smaller-than-2MiB magazine sizes strands memory.  See issue #909.
	if(FLOW_KNOBS && g_trace_depth == 0 && g_nondeterministic_random && g_nondeterministic_random->random01() < (magazine_size * Size)/FLOW_KNOBS->FAST_ALLOC_LOGGING_BYTES) {
		TraceEvent("GetMagazineSample").detail("Size", Size).backtrace();
	}
	block = (void **)::allocate(magazine_size * Size, false);
#endif

	//void** block = new void*[ magazine_size * PSize ];
	for(int i=0; i<magazine_size-1; i++) {
		block[i*PSize+1] = block[i*PSize] = &block[(i+1)*PSize];
		check( &block[i*PSize], false );
	}
		
	block[(magazine_size-1)*PSize+1] = block[(magazine_size-1)*PSize] = nullptr;
	check( &block[(magazine_size-1)*PSize], false );
	threadData.freelist = block;
	threadData.count = magazine_size;
}
template <int Size>
void FastAllocator<Size>::releaseMagazine(void* mag) {
	ASSERT(threadInitialized);
	EnterCriticalSection(&globalData()->mutex);
	globalData()->magazines.push_back(mag);
	LeaveCriticalSection(&globalData()->mutex);
}
template <int Size>
void FastAllocator<Size>::releaseThreadMagazines() {
	if(threadInitialized) {
		threadInitialized = false;
		ThreadData& thr = threadData;

		EnterCriticalSection(&globalData()->mutex);
		if (thr.freelist || thr.alternate) {
			if (thr.freelist) {
				ASSERT(thr.count > 0 && thr.count <= magazine_size);
				globalData()->partial_magazines.push_back( std::make_pair(thr.count, thr.freelist) );
				globalData()->partialMagazineUnallocatedMemory += thr.count * Size;
			}
			if (thr.alternate) {
				globalData()->magazines.push_back(thr.alternate);
			}
		}
		--globalData()->activeThreads;
		LeaveCriticalSection(&globalData()->mutex);

		thr.count = 0;
		thr.alternate = nullptr;
		thr.freelist = nullptr;
	}
}

void releaseAllThreadMagazines() {
	FastAllocator<16>::releaseThreadMagazines();
	FastAllocator<32>::releaseThreadMagazines();
	FastAllocator<64>::releaseThreadMagazines();
	FastAllocator<128>::releaseThreadMagazines();
	FastAllocator<256>::releaseThreadMagazines();
	FastAllocator<512>::releaseThreadMagazines();
	FastAllocator<1024>::releaseThreadMagazines();
	FastAllocator<2048>::releaseThreadMagazines();
	FastAllocator<4096>::releaseThreadMagazines();
	FastAllocator<8192>::releaseThreadMagazines();
}

int64_t getTotalUnusedAllocatedMemory() {
	int64_t unusedMemory = 0;

	unusedMemory += FastAllocator<16>::getApproximateMemoryUnused();
	unusedMemory += FastAllocator<32>::getApproximateMemoryUnused();
	unusedMemory += FastAllocator<64>::getApproximateMemoryUnused();
	unusedMemory += FastAllocator<128>::getApproximateMemoryUnused();
	unusedMemory += FastAllocator<256>::getApproximateMemoryUnused();
	unusedMemory += FastAllocator<512>::getApproximateMemoryUnused();
	unusedMemory += FastAllocator<1024>::getApproximateMemoryUnused();
	unusedMemory += FastAllocator<2048>::getApproximateMemoryUnused();
	unusedMemory += FastAllocator<4096>::getApproximateMemoryUnused();
	unusedMemory += FastAllocator<8192>::getApproximateMemoryUnused();

	return unusedMemory;
}

template class FastAllocator<16>;
template class FastAllocator<32>;
template class FastAllocator<64>;
template class FastAllocator<128>;
template class FastAllocator<256>;
template class FastAllocator<512>;
template class FastAllocator<1024>;
template class FastAllocator<2048>;
template class FastAllocator<4096>;
template class FastAllocator<8192>;

