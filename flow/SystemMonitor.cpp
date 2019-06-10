/*
 * SystemMonitor.cpp
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

#include "flow/flow.h"
#include "flow/Platform.h"
#include "flow/TDMetric.actor.h"
#include "flow/SystemMonitor.h"

#if defined(ALLOC_INSTRUMENTATION) && defined(__linux__)
#include <cxxabi.h>
#endif

SystemMonitorMachineState machineState;

void initializeSystemMonitorMachineState(SystemMonitorMachineState machineState) {
	::machineState = machineState;

	ASSERT(g_network);
	::machineState.monitorStartTime = now();
}

void systemMonitor() {
	static StatisticsState statState = StatisticsState();
	customSystemMonitor("ProcessMetrics", &statState, true );
}

SystemStatistics getSystemStatistics() {
	static StatisticsState statState = StatisticsState();
	const IPAddress ipAddr = machineState.ip.present() ? machineState.ip.get() : IPAddress();
	return getSystemStatistics(
		machineState.folder.present() ? machineState.folder.get() : "", &ipAddr, &statState.systemState, false);
}

#define TRACEALLOCATOR( size ) TraceEvent("MemSample").detail("Count", FastAllocator<size>::getApproximateMemoryUnused()/size).detail("TotalSize", FastAllocator<size>::getApproximateMemoryUnused()).detail("SampleCount", 1).detail("Hash", "FastAllocatedUnused" #size ).detail("Bt", "na")
#define DETAILALLOCATORMEMUSAGE( size ) detail("TotalMemory"#size, FastAllocator<size>::getTotalMemory()).detail("ApproximateUnusedMemory"#size, FastAllocator<size>::getApproximateMemoryUnused()).detail("ActiveThreads"#size, FastAllocator<size>::getActiveThreads())

SystemStatistics customSystemMonitor(std::string eventName, StatisticsState *statState, bool machineMetrics) {
	const IPAddress ipAddr = machineState.ip.present() ? machineState.ip.get() : IPAddress();
	SystemStatistics currentStats = getSystemStatistics(machineState.folder.present() ? machineState.folder.get() : "",
	                                                    &ipAddr, &statState->systemState, true);
	NetworkData netData;
	netData.init();
	if (!DEBUG_DETERMINISM && currentStats.initialized) {
		{
			TraceEvent e(eventName.c_str());
			e
				.detail("Elapsed", currentStats.elapsed)
				.detail("CPUSeconds", currentStats.processCPUSeconds)
				.detail("MainThreadCPUSeconds", currentStats.mainThreadCPUSeconds)
				.detail("UptimeSeconds", now() - machineState.monitorStartTime)
				.detail("Memory", currentStats.processMemory)
				.detail("ResidentMemory", currentStats.processResidentMemory)
				.detail("UnusedAllocatedMemory", getTotalUnusedAllocatedMemory())
				.detail("MbpsSent", ((netData.bytesSent - statState->networkState.bytesSent) * 8e-6) / currentStats.elapsed)
				.detail("MbpsReceived", ((netData.bytesReceived - statState->networkState.bytesReceived) * 8e-6) / currentStats.elapsed)
				.detail("DiskTotalBytes", currentStats.processDiskTotalBytes)
				.detail("DiskFreeBytes", currentStats.processDiskFreeBytes)
				.detail("DiskQueueDepth", currentStats.processDiskQueueDepth)
				.detail("DiskIdleSeconds", currentStats.processDiskIdleSeconds)
				.detail("DiskReads", currentStats.processDiskRead)
				.detail("DiskWrites", currentStats.processDiskWrite)
				.detail("DiskReadsCount", currentStats.processDiskReadCount)
				.detail("DiskWritesCount", currentStats.processDiskWriteCount)
				.detail("DiskWriteSectors", currentStats.processDiskWriteSectors)
				.detail("DiskReadSectors", currentStats.processDiskReadSectors)
				.detail("FileWrites", netData.countFileLogicalWrites - statState->networkState.countFileLogicalWrites)
				.detail("FileReads", netData.countFileLogicalReads - statState->networkState.countFileLogicalReads)
				.detail("CacheReadBytes", netData.countFileCacheReadBytes - statState->networkState.countFileCacheReadBytes)
				.detail("CacheFinds", netData.countFileCacheFinds - statState->networkState.countFileCacheFinds)
				.detail("CacheWritesBlocked", netData.countFileCacheWritesBlocked - statState->networkState.countFileCacheWritesBlocked)
				.detail("CacheReadsBlocked", netData.countFileCacheReadsBlocked - statState->networkState.countFileCacheReadsBlocked)
				.detail("CachePageReadsMerged", netData.countFileCachePageReadsMerged - statState->networkState.countFileCachePageReadsMerged)
				.detail("CacheWrites", netData.countFileCacheWrites - statState->networkState.countFileCacheWrites)
				.detail("CacheReads", netData.countFileCacheReads - statState->networkState.countFileCacheReads)
				.detailext("ZoneID", machineState.zoneId)
				.detailext("MachineID", machineState.machineId)
				.detail("AIOSubmitCount", netData.countAIOSubmit - statState->networkState.countAIOSubmit)
				.detail("AIOCollectCount", netData.countAIOCollect - statState->networkState.countAIOCollect)
				.detail("AIOSubmitLag", (g_network->networkMetrics.secSquaredSubmit - statState->networkMetricsState.secSquaredSubmit) / currentStats.elapsed)
				.detail("AIODiskStall", (g_network->networkMetrics.secSquaredDiskStall - statState->networkMetricsState.secSquaredDiskStall) / currentStats.elapsed)
				.detail("CurrentConnections", netData.countConnEstablished - netData.countConnClosedWithError - netData.countConnClosedWithoutError)
				.detail("ConnectionsEstablished", (double) (netData.countConnEstablished - statState->networkState.countConnEstablished) / currentStats.elapsed)
				.detail("ConnectionsClosed", ((netData.countConnClosedWithError - statState->networkState.countConnClosedWithError) + (netData.countConnClosedWithoutError - statState->networkState.countConnClosedWithoutError)) / currentStats.elapsed)
				.detail("ConnectionErrors", (netData.countConnClosedWithError - statState->networkState.countConnClosedWithError) / currentStats.elapsed)
				.trackLatest(eventName.c_str());

			TraceEvent("MemoryMetrics")
				.DETAILALLOCATORMEMUSAGE(16)
				.DETAILALLOCATORMEMUSAGE(32)
				.DETAILALLOCATORMEMUSAGE(64)
				.DETAILALLOCATORMEMUSAGE(128)
				.DETAILALLOCATORMEMUSAGE(256)
				.DETAILALLOCATORMEMUSAGE(512)
				.DETAILALLOCATORMEMUSAGE(1024)
				.DETAILALLOCATORMEMUSAGE(2048)
				.DETAILALLOCATORMEMUSAGE(4096)
				.DETAILALLOCATORMEMUSAGE(8192)
				.detail("HugeArenaMemory", g_hugeArenaMemory);

			TraceEvent n("NetworkMetrics");
			n
				.detail("CantSleep", netData.countCantSleep - statState->networkState.countCantSleep)
				.detail("WontSleep", netData.countWontSleep - statState->networkState.countWontSleep)
				.detail("Yields", netData.countYields - statState->networkState.countYields)
				.detail("YieldCalls", netData.countYieldCalls - statState->networkState.countYieldCalls)
				.detail("YieldCallsTrue", netData.countYieldCallsTrue - statState->networkState.countYieldCallsTrue)
				.detail("SlowTaskSignals", netData.countSlowTaskSignals - statState->networkState.countSlowTaskSignals)
				.detail("YieldBigStack", netData.countYieldBigStack - statState->networkState.countYieldBigStack)
				.detail("RunLoopIterations", netData.countRunLoop - statState->networkState.countRunLoop)
				.detail("TimersExecuted", netData.countTimers - statState->networkState.countTimers)
				.detail("TasksExecuted", netData.countTasks - statState->networkState.countTasks)
				.detail("ASIOEventsProcessed", netData.countASIOEvents - statState->networkState.countASIOEvents)
				.detail("ReadCalls", netData.countReads - statState->networkState.countReads)
				.detail("WriteCalls", netData.countWrites - statState->networkState.countWrites)
				.detail("ReadProbes", netData.countReadProbes - statState->networkState.countReadProbes)
				.detail("WriteProbes", netData.countWriteProbes - statState->networkState.countWriteProbes)
				.detail("PacketsRead", netData.countPacketsReceived - statState->networkState.countPacketsReceived)
				.detail("PacketsGenerated", netData.countPacketsGenerated - statState->networkState.countPacketsGenerated)
				.detail("WouldBlock", netData.countWouldBlock - statState->networkState.countWouldBlock);

			for (int i = 0; i<NetworkMetrics::SLOW_EVENT_BINS; i++)
				if (int c = g_network->networkMetrics.countSlowEvents[i] - statState->networkMetricsState.countSlowEvents[i])
					n.detail(format("SlowTask%dM", 1 << i).c_str(), c);
			for (int i = 0; i<NetworkMetrics::PRIORITY_BINS; i++)
				if (double x = g_network->networkMetrics.secSquaredPriorityBlocked[i] - statState->networkMetricsState.secSquaredPriorityBlocked[i])
					n.detail(format("S2Pri%d", g_network->networkMetrics.priorityBins[i]).c_str(), x);
		}

		if(machineMetrics) {
			TraceEvent("MachineMetrics").detail("Elapsed", currentStats.elapsed)
				.detail("MbpsSent", currentStats.machineMegabitsSent / currentStats.elapsed)
				.detail("MbpsReceived", currentStats.machineMegabitsReceived / currentStats.elapsed)
				.detail("OutSegs", currentStats.machineOutSegs)
				.detail("RetransSegs", currentStats.machineRetransSegs)
				.detail("CPUSeconds", currentStats.machineCPUSeconds)
				.detail("TotalMemory", currentStats.machineTotalRAM)
				.detail("CommittedMemory", currentStats.machineCommittedRAM)
				.detail("AvailableMemory", currentStats.machineAvailableRAM)
				.detailext("ZoneID", machineState.zoneId)
				.detailext("MachineID", machineState.machineId)
				.trackLatest("MachineMetrics");
		}
	}

#ifdef ALLOC_INSTRUMENTATION
	{
		static double firstTime = 0.0;
		if(firstTime == 0.0) firstTime = now();
		if( now() - firstTime > 10 || g_network->isSimulated() ) {
			firstTime = now();
			std::vector< std::pair<std::string, const char*> > typeNames;
			for( auto i = allocInstr.begin(); i != allocInstr.end(); ++i ) {
				std::string s;
#ifdef __linux__
				char *demangled = abi::__cxa_demangle(i->first, NULL, NULL, NULL);
				if (demangled) {
					s = demangled;
					if (StringRef(s).startsWith(LiteralStringRef("(anonymous namespace)::")))
						s = s.substr(LiteralStringRef("(anonymous namespace)::").size());
					free(demangled);
				} else
					s = i->first;
#else
				s = i->first;
				if (StringRef(s).startsWith(LiteralStringRef("class `anonymous namespace'::")))
					s = s.substr(LiteralStringRef("class `anonymous namespace'::").size());
				else if (StringRef(s).startsWith(LiteralStringRef("class ")))
					s = s.substr(LiteralStringRef("class ").size());
				else if (StringRef(s).startsWith(LiteralStringRef("struct ")))
					s = s.substr(LiteralStringRef("struct ").size());
#endif
				typeNames.push_back( std::make_pair(s, i->first) );
			}
			std::sort(typeNames.begin(), typeNames.end());
			for(int i=0; i<typeNames.size(); i++) {
				const char* n = typeNames[i].second;
				auto& f = allocInstr[n];
				if(f.maxAllocated > 10000)
					TraceEvent("AllocInstrument").detail("CurrentAlloc", f.allocCount-f.deallocCount)
						.detail("Name", typeNames[i].first.c_str());
			}

			std::unordered_map<uint32_t, BackTraceAccount> traceCounts;
			size_t memSampleSize;
			memSample_entered = true;
			{
				ThreadSpinLockHolder holder(memLock);
				traceCounts = backTraceLookup;
				memSampleSize = memSample.size();
			}
			memSample_entered = false;

			uint64_t totalSize = 0;
			uint64_t totalCount = 0;
			for( auto i = traceCounts.begin(); i != traceCounts.end(); ++i ) {
				char buf[1024];
				std::vector<void *> *frames = i->second.backTrace;
				std::string backTraceStr;
#if defined(_WIN32)
				for (int j = 1; j < frames->size(); j++) {
					_snprintf(buf, 1024, "%p ", frames->at(j));
					backTraceStr += buf;
				}
#else
				backTraceStr = platform::format_backtrace(&(*frames)[0], frames->size());
#endif

				TraceEvent("MemSample")
					.detail("Count", (int64_t)i->second.count)
					.detail("TotalSize", i->second.totalSize)
					.detail("SampleCount", i->second.sampleCount)
					.detail("Hash", format("%lld", i->first))
					.detail("Bt", backTraceStr);

				totalSize += i->second.totalSize;
				totalCount += i->second.count;
			}

			TraceEvent("MemSampleSummary")
				.detail("InverseByteSampleRatio", SAMPLE_BYTES)
				.detail("MemorySamples", memSampleSize)
				.detail("BackTraces", traceCounts.size())
				.detail("TotalSize", totalSize)
				.detail("TotalCount", totalCount);

			TraceEvent("MemSample")
				.detail("Count", traceCounts.size())
				.detail("TotalSize", traceCounts.size() * ((int)(sizeof(uint32_t) + sizeof(size_t) + sizeof(size_t))))
				.detail("SampleCount", traceCounts.size())
				.detail("Hash", "backTraces")
				.detail("Bt", "na");

			TraceEvent("MemSample")
				.detail("Count", memSampleSize)
				.detail("TotalSize", memSampleSize * ((int)(sizeof(void*) + sizeof(uint32_t) + sizeof(size_t))))
				.detail("SampleCount", memSampleSize)
				.detail("Hash", "memSamples")
				.detail("Bt", "na");
			TRACEALLOCATOR(16);
			TRACEALLOCATOR(32);
			TRACEALLOCATOR(64);
			TRACEALLOCATOR(128);
			TRACEALLOCATOR(256);
			TRACEALLOCATOR(512);
			TRACEALLOCATOR(1024);
			TRACEALLOCATOR(2048);
			TRACEALLOCATOR(4096);
			TRACEALLOCATOR(8192);
		}
	}
#endif
	statState->networkMetricsState = g_network->networkMetrics;
	statState->networkState = netData;
	return currentStats;
}
