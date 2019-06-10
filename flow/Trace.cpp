/*
 * Trace.cpp
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


#include "flow/Trace.h"
#include "flow/FileTraceLogWriter.h"
#include "flow/XmlTraceLogFormatter.h"
#include "flow/JsonTraceLogFormatter.h"
#include "flow/flow.h"
#include "flow/DeterministicRandom.h"
#include <stdlib.h>
#include <stdarg.h>
#include <cctype>
#include <time.h>

#include "flow/IThreadPool.h"
#include "flow/ThreadHelper.actor.h"
#include "flow/FastRef.h"
#include "flow/EventTypes.actor.h"
#include "flow/TDMetric.actor.h"
#include "flow/MetricSample.h"

#ifdef _WIN32
#include <windows.h>
#undef max
#undef min
#endif

int g_trace_depth = 0;

class DummyThreadPool : public IThreadPool, ReferenceCounted<DummyThreadPool> {
public:
	~DummyThreadPool() {}
	DummyThreadPool() : thread(NULL) {}
	Future<Void> getError() {
		return errors.getFuture();
	}
	void addThread( IThreadPoolReceiver* userData ) {
		ASSERT( !thread );
		thread = userData;
	}
	void post( PThreadAction action ) {
		try {
			(*action)( thread );
		} catch (Error& e) {
			errors.sendError( e );
		} catch (...) {
			errors.sendError( unknown_error() );
		}
	}
	Future<Void> stop() {
		return Void();
	}
	void addref() {
		ReferenceCounted<DummyThreadPool>::addref();
	}
	void delref() {
		ReferenceCounted<DummyThreadPool>::delref();
	}

private:
	IThreadPoolReceiver* thread;
	Promise<Void> errors;
};

struct SuppressionMap {
	struct SuppressionInfo {
		double endTime;
		int64_t suppressedEventCount;

		SuppressionInfo() : endTime(0), suppressedEventCount(0) {}
	};

	std::map<std::string, SuppressionInfo> suppressionMap;

	// Returns -1 if this event is suppressed
	int64_t checkAndInsertSuppression(std::string type, double duration) {
		ASSERT(g_network);
		if(suppressionMap.size() >= FLOW_KNOBS->MAX_TRACE_SUPPRESSIONS) {
			TraceEvent(SevWarnAlways, "ClearingTraceSuppressionMap");
			suppressionMap.clear();
		}

		auto insertion = suppressionMap.insert(std::make_pair(type, SuppressionInfo()));
		if(insertion.second || insertion.first->second.endTime <= now()) {
			int64_t suppressedEventCount = insertion.first->second.suppressedEventCount;
			insertion.first->second.endTime = now() + duration;
			insertion.first->second.suppressedEventCount = 0;
			return suppressedEventCount;
		}
		else {
			++insertion.first->second.suppressedEventCount;
			return -1;
		}
	}
};

TraceBatch g_traceBatch;
trace_clock_t g_trace_clock = TRACE_CLOCK_NOW;
IRandom* trace_random = NULL;

LatestEventCache latestEventCache;
SuppressionMap suppressedEvents;

static TransientThresholdMetricSample<Standalone<StringRef>> *traceEventThrottlerCache;
static const char *TRACE_EVENT_THROTTLE_STARTING_TYPE = "TraceEventThrottle_";
static const char *TRACE_EVENT_INVALID_SUPPRESSION = "InvalidSuppression_";
static int TRACE_LOG_MAX_PREOPEN_BUFFER = 1000000;
static int TRACE_EVENT_MAX_SIZE = 4000;

struct TraceLog {
	Reference<ITraceLogFormatter> formatter;

private:
	Reference<ITraceLogWriter> logWriter;
	std::vector<TraceEventFields> eventBuffer;
	int loggedLength;
	int bufferLength;
	bool opened;
	int64_t preopenOverflowCount;
	std::string basename;
	std::string logGroup;

	std::string directory;
	std::string processName;
	Optional<NetworkAddress> localAddress;

	Reference<IThreadPool> writer;
	uint64_t rollsize;
	Mutex mutex;

	EventMetricHandle<TraceEventNameID> SevErrorNames;
	EventMetricHandle<TraceEventNameID> SevWarnAlwaysNames;
	EventMetricHandle<TraceEventNameID> SevWarnNames;
	EventMetricHandle<TraceEventNameID> SevInfoNames;
	EventMetricHandle<TraceEventNameID> SevDebugNames;

	struct RoleInfo {
		std::map<std::string, int> roles;
		std::string rolesString;

		void refreshRolesString() {
			rolesString = "";
			for(auto itr : roles) {
				if(!rolesString.empty()) {
					rolesString += ",";
				}
				rolesString += itr.first;
			}
		}
	};

	RoleInfo roleInfo;
	std::map<NetworkAddress, RoleInfo> roleInfoMap;

	RoleInfo& mutateRoleInfo() {
		ASSERT(g_network);

		if(g_network->isSimulated()) {
			return roleInfoMap[g_network->getLocalAddress()];
		}
		
		return roleInfo;
	}

public:
	bool logTraceEventMetrics;

	void initMetrics()
	{
		SevErrorNames.init(LiteralStringRef("TraceEvents.SevError"));
		SevWarnAlwaysNames.init(LiteralStringRef("TraceEvents.SevWarnAlways"));
		SevWarnNames.init(LiteralStringRef("TraceEvents.SevWarn"));
		SevInfoNames.init(LiteralStringRef("TraceEvents.SevInfo"));
		SevDebugNames.init(LiteralStringRef("TraceEvents.SevDebug"));
		logTraceEventMetrics = true;
	}

	struct BarrierList : ThreadSafeReferenceCounted<BarrierList> {
		BarrierList() : ntriggered(0) {}
		void push( ThreadFuture<Void> f ) {
			MutexHolder h(mutex);
			barriers.push_back(f);
		}
		void pop() {
			MutexHolder h(mutex);
			unsafeTrigger(0);
			barriers.pop_front();
			if (ntriggered) ntriggered--;
		}
		void triggerAll() {
			MutexHolder h(mutex);
			for(int i=ntriggered; i<barriers.size(); i++)
				unsafeTrigger(i);
			ntriggered = barriers.size();
		}
	private:
		Mutex mutex;
		Deque< ThreadFuture<Void> > barriers;
		int ntriggered;
		void unsafeTrigger(int i) {
			auto b = ((ThreadSingleAssignmentVar<Void>*)barriers[i].getPtr());
			if (!b->isReady())
				b->send(Void());
		}
	};

	Reference<BarrierList> barriers;

	struct WriterThread : IThreadPoolReceiver {
		WriterThread( Reference<BarrierList> barriers, Reference<ITraceLogWriter> logWriter, Reference<ITraceLogFormatter> formatter ) 
			: barriers(barriers), logWriter(logWriter), formatter(formatter) {}

		virtual void init() {}

		Reference<ITraceLogWriter> logWriter;
		Reference<ITraceLogFormatter> formatter;
		Reference<BarrierList> barriers;

		struct Open : TypedAction<WriterThread,Open> {
			virtual double getTimeEstimate() { return 0; }
		};
		void action( Open& o ) {
			logWriter->open();
			logWriter->write(formatter->getHeader());
		}

		struct Close : TypedAction<WriterThread,Close> {
			virtual double getTimeEstimate() { return 0; }
		};
		void action( Close& c ) {
			logWriter->write(formatter->getFooter());
			logWriter->close();
		}

		struct Roll : TypedAction<WriterThread,Roll> {
			virtual double getTimeEstimate() { return 0; }
		};
		void action( Roll& c ) {
			logWriter->write(formatter->getFooter());
			logWriter->roll();
			logWriter->write(formatter->getHeader());
		}

		struct Barrier : TypedAction<WriterThread, Barrier> {
			virtual double getTimeEstimate() { return 0; }
		};
		void action( Barrier& a ) {
			barriers->pop();
		}

		struct WriteBuffer : TypedAction<WriterThread, WriteBuffer> {
			std::vector<TraceEventFields> events;

			WriteBuffer(std::vector<TraceEventFields> events) : events(events) {}
			virtual double getTimeEstimate() { return .001; }
		};
		void action( WriteBuffer& a ) {
			for(auto event : a.events) {
				event.validateFormat();
				logWriter->write(formatter->formatEvent(event));
			}

			if(FLOW_KNOBS->TRACE_SYNC_ENABLED) {
				logWriter->sync();
			}
		}
	};

	TraceLog() : bufferLength(0), loggedLength(0), opened(false), preopenOverflowCount(0), barriers(new BarrierList), logTraceEventMetrics(false), formatter(new XmlTraceLogFormatter()) {}

	bool isOpen() const { return opened; }

	void open( std::string const& directory, std::string const& processName, std::string logGroup, std::string const& timestamp, uint64_t rs, uint64_t maxLogsSize, Optional<NetworkAddress> na ) {
		ASSERT( !writer && !opened );

		this->directory = directory;
		this->processName = processName;
		this->logGroup = logGroup;
		this->localAddress = na;

		basename = format("%s/%s.%s.%s", directory.c_str(), processName.c_str(), timestamp.c_str(), g_random->randomAlphaNumeric(6).c_str());
		logWriter = Reference<ITraceLogWriter>(new FileTraceLogWriter(directory, processName, basename, formatter->getExtension(), maxLogsSize, [this](){ barriers->triggerAll(); }));

		if ( g_network->isSimulated() )
			writer = Reference<IThreadPool>(new DummyThreadPool());
		else
			writer = createGenericThreadPool();
		writer->addThread( new WriterThread(barriers, logWriter, formatter) );

		rollsize = rs;

		auto a = new WriterThread::Open;
		writer->post(a);

		MutexHolder holder(mutex);
		if(g_network->isSimulated()) {
			// We don't support early trace logs in simulation.
			// This is because we don't know if we're being simulated prior to the network being created, which causes two ambiguities:
			//
			// 1. We need to employ two different methods to determine the time of an event prior to the network starting for real-world and simulated runs.
			// 2. Simulated runs manually insert the Machine field at TraceEvent creation time. Real-world runs add this field at write time.
			//
			// Without the ability to resolve the ambiguity, we've chosen to always favor the real-world approach and not support such events in simulation.
			eventBuffer.clear();
		}

		for(TraceEventFields &fields : eventBuffer) {
			annotateEvent(fields);
		}

		opened = true;
		if(preopenOverflowCount > 0) {
			TraceEvent(SevWarn, "TraceLogPreopenOverflow").detail("OverflowEventCount", preopenOverflowCount);
			preopenOverflowCount = 0;
		}
	}

	void annotateEvent( TraceEventFields &fields ) {
		if(localAddress.present()) {
			fields.addField("Machine", formatIpPort(localAddress.get().ip, localAddress.get().port));
		}

		fields.addField("LogGroup", logGroup);

		RoleInfo const& r = mutateRoleInfo();
		if(r.rolesString.size() > 0) {
			fields.addField("Roles", r.rolesString);
		}
	}

	void writeEvent( TraceEventFields fields, std::string trackLatestKey, bool trackError ) {
		MutexHolder hold(mutex);

		if(opened) {
			annotateEvent(fields);
		}

		if(!trackLatestKey.empty()) {
			fields.addField("TrackLatestType", "Original");
		}

		if(!isOpen() && (preopenOverflowCount > 0 || bufferLength + fields.sizeBytes() > TRACE_LOG_MAX_PREOPEN_BUFFER)) {
			++preopenOverflowCount;
			return;
		}

		// FIXME: What if we are using way too much memory for buffer?
		eventBuffer.push_back(fields);
		bufferLength += fields.sizeBytes();

		if(trackError) {
			latestEventCache.setLatestError(fields);
		}
		if(!trackLatestKey.empty()) {
			latestEventCache.set(trackLatestKey, fields);
		}
	}

	void log(int severity, const char *name, UID id, uint64_t event_ts)
	{
		if(!logTraceEventMetrics)
			return;

		EventMetricHandle<TraceEventNameID> *m = NULL;
		switch(severity)
		{
			case SevError:       m = &SevErrorNames;      break;
			case SevWarnAlways:  m = &SevWarnAlwaysNames; break;
			case SevWarn:        m = &SevWarnNames;       break;
			case SevInfo:        m = &SevInfoNames;       break;
			case SevDebug:       m = &SevDebugNames;      break;
			default:
			break;
		}
		if(m != NULL)
		{
			(*m)->name = StringRef((uint8_t*)name, strlen(name));
			(*m)->id = id.toString();
			(*m)->log(event_ts);
		}
	}

	ThreadFuture<Void> flush() {
		traceEventThrottlerCache->poll();

		MutexHolder hold(mutex);
		bool roll = false;
		if (!eventBuffer.size()) return Void(); // SOMEDAY: maybe we still roll the tracefile here?

		if (rollsize && bufferLength + loggedLength > rollsize) // SOMEDAY: more conditions to roll
			roll = true;

		auto a = new WriterThread::WriteBuffer( std::move(eventBuffer) );
		loggedLength += bufferLength;
		eventBuffer = std::vector<TraceEventFields>();
		bufferLength = 0;
		writer->post( a );

		if (roll) {
			auto o = new WriterThread::Roll;
			writer->post(o);

			std::vector<TraceEventFields> events = latestEventCache.getAllUnsafe();
			for (int idx = 0; idx < events.size(); idx++) {
				if(events[idx].size() > 0) {
					TraceEventFields rolledFields;
					for(auto itr = events[idx].begin(); itr != events[idx].end(); ++itr) {
						if(itr->first == "Time") {
							rolledFields.addField("Time", format("%.6f", (g_trace_clock == TRACE_CLOCK_NOW) ? now() : timer()));
							rolledFields.addField("OriginalTime", itr->second);
						}
						else if(itr->first == "TrackLatestType") {
							rolledFields.addField("TrackLatestType", "Rolled");
						}
						else {
							rolledFields.addField(itr->first, itr->second);
						}
					}

					eventBuffer.push_back(rolledFields);
				}
			}

			loggedLength = 0;
		}

		ThreadFuture<Void> f(new ThreadSingleAssignmentVar<Void>);
		barriers->push(f);
		writer->post( new WriterThread::Barrier );

		return f;
	}

	void close() {
		if (opened) {
			MutexHolder hold(mutex);

			// Write remaining contents
			auto a = new WriterThread::WriteBuffer( std::move(eventBuffer) );
			loggedLength += bufferLength;
			eventBuffer = std::vector<TraceEventFields>();
			bufferLength = 0;
			writer->post( a );

			auto c = new WriterThread::Close();
			writer->post( c );

			ThreadFuture<Void> f(new ThreadSingleAssignmentVar<Void>);
			barriers->push(f);
			writer->post( new WriterThread::Barrier );

			f.getBlocking();

			opened = false;
		}
	}

	void addRole(std::string role) {
		MutexHolder holder(mutex);

		RoleInfo &r = mutateRoleInfo();
		++r.roles[role];
		r.refreshRolesString();
	}

	void removeRole(std::string role) {
		MutexHolder holder(mutex);

		RoleInfo &r = mutateRoleInfo();

		auto itr = r.roles.find(role);
		ASSERT(itr != r.roles.end() || (g_network->isSimulated() && g_network->getLocalAddress() == NetworkAddress()));

		if(itr != r.roles.end() && --(*itr).second == 0) {
			r.roles.erase(itr);
			r.refreshRolesString();
		}
	}

	~TraceLog() {
		close();
		if (writer) writer->addref(); // FIXME: We are not shutting down the writer thread at all, because the ThreadPool shutdown mechanism is blocking (necessarily waits for current work items to finish) and we might not be able to finish everything.
	}
};

NetworkAddress getAddressIndex() {
// ahm
//	if( g_network->isSimulated() )
//		return g_simulator.getCurrentProcess()->address;
//	else
	return g_network->getLocalAddress();
}

// This does not check for simulation, and as such is not safe for external callers
void clearPrefix_internal( std::map<std::string, TraceEventFields>& data, std::string prefix ) {
	auto first = data.lower_bound( prefix );
	auto last = data.lower_bound( strinc( prefix ).toString() );
	data.erase( first, last );
}

void LatestEventCache::clear( std::string prefix ) {
	clearPrefix_internal( latest[getAddressIndex()], prefix );
}

void LatestEventCache::clear() {
	latest[getAddressIndex()].clear();
}

void LatestEventCache::set( std::string tag, const TraceEventFields& contents ) {
	latest[getAddressIndex()][tag] = contents;
}

TraceEventFields LatestEventCache::get( std::string tag ) {
	return latest[getAddressIndex()][tag];
}

std::vector<TraceEventFields> allEvents( std::map<std::string, TraceEventFields> const& data ) {
	std::vector<TraceEventFields> all;
	for(auto it = data.begin(); it != data.end(); it++) {
		all.push_back( it->second );
	}
	return all;
}

std::vector<TraceEventFields> LatestEventCache::getAll() {
	return allEvents( latest[getAddressIndex()] );
}

// if in simulation, all events from all machines will be returned
std::vector<TraceEventFields> LatestEventCache::getAllUnsafe() {
	std::vector<TraceEventFields> all;
	for(auto it = latest.begin(); it != latest.end(); ++it) {
		auto m = allEvents( it->second );
		all.insert( all.end(), m.begin(), m.end() );
	}
	return all;
}

void LatestEventCache::setLatestError( const TraceEventFields& contents ) {
	if(TraceEvent::isNetworkThread()) { // The latest event cache doesn't track errors that happen on other threads
		latestErrors[getAddressIndex()] = contents;
	}
}

TraceEventFields LatestEventCache::getLatestError() {
	return latestErrors[getAddressIndex()];
}

static TraceLog g_traceLog;

namespace {
template <bool validate>
bool traceFormatImpl(std::string& format) {
	std::transform(format.begin(), format.end(), format.begin(), ::tolower);
	if (format == "xml") {
		if (!validate) {
			g_traceLog.formatter = Reference<ITraceLogFormatter>(new XmlTraceLogFormatter());
		}
		return true;
	} else if (format == "json") {
		if (!validate) {
			g_traceLog.formatter = Reference<ITraceLogFormatter>(new JsonTraceLogFormatter());
		}
		return true;
	} else {
		if (!validate) {
			g_traceLog.formatter = Reference<ITraceLogFormatter>(new XmlTraceLogFormatter());
		}
		return false;
	}
}
} // namespace

bool selectTraceFormatter(std::string format) {
	ASSERT(!g_traceLog.isOpen());
	bool recognized = traceFormatImpl</*validate*/ false>(format);
	if (!recognized) {
		TraceEvent(SevWarnAlways, "UnrecognizedTraceFormat").detail("format", format);
	}
	return recognized;
}

bool validateTraceFormat(std::string format) {
	return traceFormatImpl</*validate*/ true>(format);
}

ThreadFuture<Void> flushTraceFile() {
	if (!g_traceLog.isOpen())
		return Void();
	return g_traceLog.flush();
}

void flushTraceFileVoid() {
	if ( g_network && g_network->isSimulated() )
		flushTraceFile();
	else {
		flushTraceFile().getBlocking();
	}
}

void openTraceFile(const NetworkAddress& na, uint64_t rollsize, uint64_t maxLogsSize, std::string directory, std::string baseOfBase, std::string logGroup) {
	if(g_traceLog.isOpen())
		return;

	if(directory.empty())
		directory = ".";

	if (baseOfBase.empty())
		baseOfBase = "trace";

	std::string ip = na.ip.toString();
	std::replace(ip.begin(), ip.end(), ':', '_'); // For IPv6, Windows doesn't accept ':' in filenames.
	std::string baseName = format("%s.%s.%d", baseOfBase.c_str(), ip.c_str(), na.port);
	g_traceLog.open( directory, baseName, logGroup, format("%lld", time(NULL)), rollsize, maxLogsSize, !g_network->isSimulated() ? na : Optional<NetworkAddress>());

	uncancellable(recurring(&flushTraceFile, FLOW_KNOBS->TRACE_FLUSH_INTERVAL, TaskFlushTrace));
	g_traceBatch.dump();
}

void initTraceEventMetrics() {
	g_traceLog.initMetrics();
}

void closeTraceFile() {
	g_traceLog.close();
}

bool traceFileIsOpen() {
	return g_traceLog.isOpen();
}

void addTraceRole(std::string role) {
	g_traceLog.addRole(role);
}

void removeTraceRole(std::string role) {
	g_traceLog.removeRole(role);
}

TraceEvent::TraceEvent( const char* type, UID id ) : id(id), type(type), severity(SevInfo), initialized(false), enabled(true) {
	g_trace_depth++;
}
TraceEvent::TraceEvent( Severity severity, const char* type, UID id ) : id(id), type(type), severity(severity), initialized(false), enabled(true) {
	g_trace_depth++;
}
TraceEvent::TraceEvent( TraceInterval& interval, UID id ) : id(id), type(interval.type), severity(interval.severity), initialized(false), enabled(true) {
	g_trace_depth++;
	init(interval);
}
TraceEvent::TraceEvent( Severity severity, TraceInterval& interval, UID id ) : id(id), type(interval.type), severity(severity), initialized(false), enabled(true) {
	g_trace_depth++;
	init(interval);
}

bool TraceEvent::init( TraceInterval& interval ) {
	bool result = init();
	switch (interval.count++) {
		case 0: { detail("BeginPair", interval.pairID); break; }
		case 1: { detail("EndPair", interval.pairID); break; }
		default: ASSERT(false);
	}
	return result;
}

bool TraceEvent::init() {
	if(initialized) {
		return enabled;
	}
	initialized = true;

	ASSERT(*type != '\0');
	enabled = enabled && ( !g_network || severity >= FLOW_KNOBS->MIN_TRACE_SEVERITY );

	// Backstop to throttle very spammy trace events
	if (enabled && g_network && !g_network->isSimulated() && severity > SevDebug && isNetworkThread()) {
		if (traceEventThrottlerCache->isAboveThreshold(StringRef((uint8_t*)type, strlen(type)))) {
			enabled = false;
			TraceEvent(SevWarnAlways, std::string(TRACE_EVENT_THROTTLE_STARTING_TYPE).append(type).c_str()).suppressFor(5);
		}
		else {
			traceEventThrottlerCache->addAndExpire(StringRef((uint8_t*)type, strlen(type)), 1, now() + FLOW_KNOBS->TRACE_EVENT_THROTTLER_SAMPLE_EXPIRY);
		}
	}

	if(enabled) {
		tmpEventMetric = new DynamicEventMetric(MetricNameRef());

		double time;
		if(g_trace_clock == TRACE_CLOCK_NOW) {
			if(!g_network) {
				static double preNetworkTime = timer_monotonic();
				time = preNetworkTime;
			}
			else {
				time = now();
			}
		}
		else {
			time = timer();
		}

		if(err.isValid() && err.isInjectedFault() && severity == SevError) {
			severity = SevWarnAlways;
		}

		detail("Severity", severity);
		detailf("Time", "%.6f", time);
		detail("Type", type);
		if(g_network && g_network->isSimulated()) {
			NetworkAddress local = g_network->getLocalAddress();
			detail("Machine", formatIpPort(local.ip, local.port));
		}
		detail("ID", id);
		if(err.isValid()) {
			if (err.isInjectedFault()) {
				detail("ErrorIsInjectedFault", true);
			}
			detail("Error", err.name());
			detail("ErrorDescription", err.what());
			detail("ErrorCode", err.code());
		}
	} else {
		tmpEventMetric = nullptr;
	}

	return enabled;
}

TraceEvent& TraceEvent::error(class Error const& error, bool includeCancelled) {
	if(enabled) {
		if (error.code() != error_code_actor_cancelled || includeCancelled) {
			err = error;
			if (initialized) {
				if (error.isInjectedFault()) {
					detail("ErrorIsInjectedFault", true);
					if(severity == SevError) severity = SevWarnAlways;
				}
				detail("Error", error.name());
				detail("ErrorDescription", error.what());
				detail("ErrorCode", error.code());
			}
		} else {
			if (initialized) {
				TraceEvent(g_network && g_network->isSimulated() ? SevError : SevWarnAlways, std::string(TRACE_EVENT_INVALID_SUPPRESSION).append(type).c_str()).suppressFor(5);
			} else {
				enabled = false;
			}
		}
	}
	return *this;
}

TraceEvent& TraceEvent::detailImpl( std::string&& key, std::string&& value, bool writeEventMetricField) {
	init();
	if (enabled) {
		if( value.size() > 495 ) {
			value = value.substr(0, 495) + "...";
		}

		if(writeEventMetricField) {
			tmpEventMetric->setField(key.c_str(), Standalone<StringRef>(StringRef(value)));
		}

		fields.addField(std::move(key), std::move(value));

		if(fields.sizeBytes() > TRACE_EVENT_MAX_SIZE) {
			TraceEvent(g_network && g_network->isSimulated() ? SevError : SevWarnAlways, "TraceEventOverflow").detail("TraceFirstBytes", fields.toString().substr(300));
			enabled = false;
		}
	}
	return *this;
}

TraceEvent& TraceEvent::detail( std::string key, std::string value ) {
	return detailImpl(std::move(key), std::move(value));
}
TraceEvent& TraceEvent::detail( std::string key, double value ) {
	init();
	if(enabled)
		tmpEventMetric->setField(key.c_str(), value);
	return detailfNoMetric( std::move(key), "%g", value );
}
TraceEvent& TraceEvent::detail( std::string key, int value ) {
	init();
	if(enabled)
		tmpEventMetric->setField(key.c_str(), (int64_t)value);
	return detailfNoMetric( std::move(key), "%d", value );
}
TraceEvent& TraceEvent::detail( std::string key, unsigned value ) {
	init();
	if(enabled)
		tmpEventMetric->setField(key.c_str(), (int64_t)value);
	return detailfNoMetric( std::move(key), "%u", value );
}
TraceEvent& TraceEvent::detail( std::string key, long int value ) {
	init();
	if(enabled)
		tmpEventMetric->setField(key.c_str(), (int64_t)value);
	return detailfNoMetric( std::move(key), "%ld", value );
}
TraceEvent& TraceEvent::detail( std::string key, long unsigned int value ) {
	init();
	if(enabled)
		tmpEventMetric->setField(key.c_str(), (int64_t)value);
	return detailfNoMetric( std::move(key), "%lu", value );
}
TraceEvent& TraceEvent::detail( std::string key, long long int value ) {
	init();
	if(enabled)
		tmpEventMetric->setField(key.c_str(), (int64_t)value);
	return detailfNoMetric( std::move(key), "%lld", value );
}
TraceEvent& TraceEvent::detail( std::string key, long long unsigned int value ) {
	init();
	if(enabled)
		tmpEventMetric->setField(key.c_str(), (int64_t)value);
	return detailfNoMetric( std::move(key), "%llu", value );
}
TraceEvent& TraceEvent::detail( std::string key, const NetworkAddress& value ) {
	return detailImpl( std::move(key), value.toString() );
}
TraceEvent& TraceEvent::detail( std::string key, const IPAddress& value ) {
	return detailImpl( std::move(key), value.toString() );
}
TraceEvent& TraceEvent::detail( std::string key, const UID& value ) {
	return detailf( std::move(key), "%016llx", value.first() );  // SOMEDAY: Log entire value?  We also do this explicitly in some "lists" in various individual TraceEvent calls
}
TraceEvent& TraceEvent::detailext( std::string key, StringRef const& value ) {
	return detailImpl(std::move(key), value.printable());
}
TraceEvent& TraceEvent::detailext( std::string key, const Optional<Standalone<StringRef>>& value ) {
	return detailImpl(std::move(key), (value.present()) ? value.get().printable() : "[not set]");
}
TraceEvent& TraceEvent::detailf( std::string key, const char* valueFormat, ... ) {
	if (enabled) {
		va_list args;
		va_start(args, valueFormat);
		std::string value;
		int result = vsformat(value, valueFormat, args);
		va_end(args);

		ASSERT(result >= 0);
		detailImpl(std::move(key), std::move(value));
	}
	return *this;
}
TraceEvent& TraceEvent::detailfNoMetric( std::string&& key, const char* valueFormat, ... ) {
	if (enabled) {
		va_list args;
		va_start(args, valueFormat);
		std::string value;
		int result = vsformat(value, valueFormat, args);
		va_end(args);

		ASSERT(result >= 0);
		detailImpl(std::move(key), std::move(value), false); // Do NOT write this detail to the event metric, caller of detailfNoMetric should do that itself with the appropriate value type
	}
	return *this;
}

TraceEvent& TraceEvent::trackLatest( const char *trackingKey ){
	this->trackingKey = trackingKey;
	ASSERT( this->trackingKey.size() != 0 && this->trackingKey[0] != '/' && this->trackingKey[0] != '\\');
	return *this;
}

TraceEvent& TraceEvent::sample( double sampleRate, bool logSampleRate ) {
	if(enabled) {
		if(initialized) {
			TraceEvent(g_network && g_network->isSimulated() ? SevError : SevWarnAlways, std::string(TRACE_EVENT_INVALID_SUPPRESSION).append(type).c_str()).suppressFor(5);
			return *this;
		}

		if(!g_random) {
			sampleRate = 1.0;
		}
		else {
			enabled = enabled && g_random->random01() < sampleRate;
		}

		if(enabled && logSampleRate) {
			detail("SampleRate", sampleRate);
		}
	}

	return *this;
}

TraceEvent& TraceEvent::suppressFor( double duration, bool logSuppressedEventCount ) {
	if(enabled) {
		if(initialized) {
			TraceEvent(g_network && g_network->isSimulated() ? SevError : SevWarnAlways, std::string(TRACE_EVENT_INVALID_SUPPRESSION).append(type).c_str()).suppressFor(5);
			return *this;
		}

		if(g_network) {
			if(isNetworkThread()) {
				int64_t suppressedEventCount = suppressedEvents.checkAndInsertSuppression(type, duration);
				enabled = enabled && suppressedEventCount >= 0;
				if(enabled && logSuppressedEventCount) {
					detail("SuppressedEventCount", suppressedEventCount);
				}
			}
			else {
				TraceEvent(SevWarnAlways, "SuppressionFromNonNetworkThread").detail("Type", type);
				detail("__InvalidSuppression__", ""); // Choosing a detail name that is unlikely to collide with other names
			}
		}
		init(); //we do not want any future calls on this trace event to disable it, because we have already counted it towards our suppression budget
	}

	return *this;
}

TraceEvent& TraceEvent::GetLastError() {
#ifdef _WIN32
	return detailf("WinErrorCode", "%x", ::GetLastError());
#elif defined(__unixish__)
	return detailf("UnixErrorCode", "%x", errno).detail("UnixError", strerror(errno));
#endif
}

// We're cheating in counting, as in practice, we only use {10,20,30,40}.
static_assert(SevMaxUsed / 10 + 1 == 5, "Please bump eventCounts[5] to SevMaxUsed/10+1");
unsigned long TraceEvent::eventCounts[5] = {0,0,0,0,0};

unsigned long TraceEvent::CountEventsLoggedAt(Severity sev) {
  return TraceEvent::eventCounts[sev/10];
}

TraceEvent& TraceEvent::backtrace(const std::string& prefix) {
	if (this->severity == SevError || !enabled) return *this; // We'll backtrace this later in ~TraceEvent
	return detail(prefix + "Backtrace", platform::get_backtrace());
}

TraceEvent::~TraceEvent() {
	init();
	try {
		if (enabled) {
			if (this->severity == SevError) {
				severity = SevInfo;
				backtrace();
				severity = SevError;
			}

			TraceEvent::eventCounts[severity/10]++;
			g_traceLog.writeEvent( fields, trackingKey, severity > SevWarnAlways );

			if (g_traceLog.isOpen()) {
				// Log Metrics
				if(g_traceLog.logTraceEventMetrics && isNetworkThread()) {
					// Get the persistent Event Metric representing this trace event and push the fields (details) accumulated in *this to it and then log() it.
					// Note that if the event metric is disabled it won't actually be logged BUT any new fields added to it will be registered.
					// If the event IS logged, a timestamp will be returned, if not then 0.  Either way, pass it through to be used if possible
					// in the Sev* event metrics.

					uint64_t event_ts = DynamicEventMetric::getOrCreateInstance(format("TraceEvent.%s", type), StringRef(), true)->setFieldsAndLogFrom(tmpEventMetric);
					g_traceLog.log(severity, type, id, event_ts);
				}
			}
		}
	} catch( Error &e ) {
		TraceEvent(SevError, "TraceEventDestructorError").error(e,true);
	}
	delete tmpEventMetric;
	g_trace_depth--;
}

thread_local bool TraceEvent::networkThread = false;

void TraceEvent::setNetworkThread() {
	traceEventThrottlerCache = new TransientThresholdMetricSample<Standalone<StringRef>>(FLOW_KNOBS->TRACE_EVENT_METRIC_UNITS_PER_SAMPLE, FLOW_KNOBS->TRACE_EVENT_THROTTLER_MSG_LIMIT);
	networkThread = true;
}

bool TraceEvent::isNetworkThread() {
	return networkThread;
}

TraceInterval& TraceInterval::begin() {
	pairID = trace_random->randomUniqueID();
	count = 0;
	return *this;
}

void TraceBatch::addEvent( const char *name, uint64_t id, const char *location ) {
	eventBatch.push_back( EventInfo(g_trace_clock == TRACE_CLOCK_NOW ? now() : timer(), name, id, location));
	if( g_network->isSimulated() || FLOW_KNOBS->AUTOMATIC_TRACE_DUMP )
		dump();
}

void TraceBatch::addAttach( const char *name, uint64_t id, uint64_t to ) {
	attachBatch.push_back( AttachInfo(g_trace_clock == TRACE_CLOCK_NOW ? now() : timer(), name, id, to));
	if( g_network->isSimulated() || FLOW_KNOBS->AUTOMATIC_TRACE_DUMP )
		dump();
}

void TraceBatch::addBuggify( int activated, int line, std::string file ) {
	if( g_network ) {
		buggifyBatch.push_back( BuggifyInfo(g_trace_clock == TRACE_CLOCK_NOW ? now() : timer(), activated, line, file));
		if( g_network->isSimulated() || FLOW_KNOBS->AUTOMATIC_TRACE_DUMP )
			dump();
	} else {
		buggifyBatch.push_back( BuggifyInfo(0, activated, line, file));
	}
}

void TraceBatch::dump() {
	if (!g_traceLog.isOpen())
		return;
	std::string machine;
	if(g_network->isSimulated()) {
		NetworkAddress local = g_network->getLocalAddress();
		machine = formatIpPort(local.ip, local.port);
	}

	for(int i = 0; i < attachBatch.size(); i++) {
		if(g_network->isSimulated()) {
			attachBatch[i].fields.addField("Machine", machine);
		}
		g_traceLog.writeEvent(attachBatch[i].fields, "", false);
	}

	for(int i = 0; i < eventBatch.size(); i++) {
		if(g_network->isSimulated()) {
			eventBatch[i].fields.addField("Machine", machine);
		}
		g_traceLog.writeEvent(eventBatch[i].fields, "", false);
	}

	for(int i = 0; i < buggifyBatch.size(); i++) {
		if(g_network->isSimulated()) {
			buggifyBatch[i].fields.addField("Machine", machine);
		}
		g_traceLog.writeEvent(buggifyBatch[i].fields, "", false);
	}

	g_traceLog.flush();
	eventBatch.clear();
	attachBatch.clear();
	buggifyBatch.clear();
}

TraceBatch::EventInfo::EventInfo(double time, const char *name, uint64_t id, const char *location) {
	fields.addField("Severity", format("%d", (int)SevInfo));
	fields.addField("Time", format("%.6f", time));
	fields.addField("Type", name);
	fields.addField("ID", format("%016" PRIx64, id));
	fields.addField("Location", location);
}

TraceBatch::AttachInfo::AttachInfo(double time, const char *name, uint64_t id, uint64_t to) {
	fields.addField("Severity", format("%d", (int)SevInfo));
	fields.addField("Time", format("%.6f", time));
	fields.addField("Type", name);
	fields.addField("ID", format("%016" PRIx64, id));
	fields.addField("To", format("%016" PRIx64, to));
}

TraceBatch::BuggifyInfo::BuggifyInfo(double time, int activated, int line, std::string file) {
	fields.addField("Severity", format("%d", (int)SevInfo));
	fields.addField("Time", format("%.6f", time));
	fields.addField("Type", "BuggifySection");
	fields.addField("Activated", format("%d", activated));
	fields.addField("File", std::move(file));
	fields.addField("Line", format("%d", line));
}

TraceEventFields::TraceEventFields() : bytes(0) {}

void TraceEventFields::addField(const std::string& key, const std::string& value) {
	bytes += key.size() + value.size();
	fields.push_back(std::make_pair(key, value));
}

void TraceEventFields::addField(std::string&& key, std::string&& value) {
	bytes += key.size() + value.size();
	fields.push_back(std::make_pair(std::move(key), std::move(value)));
}

size_t TraceEventFields::size() const {
	return fields.size();
}

size_t TraceEventFields::sizeBytes() const {
	return bytes;
}

TraceEventFields::FieldIterator TraceEventFields::begin() const {
	return fields.cbegin();
}

TraceEventFields::FieldIterator TraceEventFields::end() const {
	return fields.cend();
}

const TraceEventFields::Field &TraceEventFields::operator[] (int index) const {
	ASSERT(index >= 0 && index < size());
	return fields.at(index);
}

bool TraceEventFields::tryGetValue(std::string key, std::string &outValue) const {
	for(auto itr = begin(); itr != end(); ++itr) {
		if(itr->first == key) {
			outValue = itr->second;
			return true;
		}
	}

	return false;
}

std::string TraceEventFields::getValue(std::string key) const {
	std::string value;
	if(tryGetValue(key, value)) {
		return value;
	}
	else {
		TraceEvent ev(SevWarn, "TraceEventFieldNotFound");
		ev.suppressFor(1.0);
		if(tryGetValue("Type", value)) {
			ev.detail("Event", value);
		}
		ev.detail("FieldName", key);

		throw attribute_not_found();
	}
}

namespace {
void parseNumericValue(std::string const& s, double &outValue, bool permissive = false) {
	double d = 0;
	int consumed = 0;
	int r = sscanf(s.c_str(), "%lf%n", &d, &consumed);
	if (r == 1 && (consumed == s.size() || permissive)) {
		outValue = d;
		return;
	}

	throw attribute_not_found();
}

void parseNumericValue(std::string const& s, int &outValue, bool permissive = false) {
	long long int iLong = 0;
	int consumed = 0;
	int r = sscanf(s.c_str(), "%lld%n", &iLong, &consumed);
	if (r == 1 && (consumed == s.size() || permissive)) {
		if (std::numeric_limits<int>::min() <= iLong && iLong <= std::numeric_limits<int>::max()) {
			outValue = (int)iLong;  // Downcast definitely safe
			return;
		}
		else {
			throw attribute_too_large();
		}
	}

	throw attribute_not_found();
}

void parseNumericValue(std::string const& s, int64_t &outValue, bool permissive = false) {
	long long int i = 0;
	int consumed = 0;
	int r = sscanf(s.c_str(), "%lld%n", &i, &consumed);
	if (r == 1 && (consumed == s.size() || permissive)) {
		outValue = i;
		return;
	}

	throw attribute_not_found();
}

template<class T>
T getNumericValue(TraceEventFields const& fields, std::string key, bool permissive) {
	std::string field = fields.getValue(key);

	try {
		T value;
		parseNumericValue(field, value, permissive);
		return value;
	}
	catch(Error &e) {
		std::string type;

		TraceEvent ev(SevWarn, "ErrorParsingNumericTraceEventField");
		ev.error(e);
		if(fields.tryGetValue("Type", type)) {
			ev.detail("Event", type);
		}
		ev.detail("FieldName", key);
		ev.detail("FieldValue", field);

		throw;
	}
}
} // namespace

int TraceEventFields::getInt(std::string key, bool permissive) const {
	return getNumericValue<int>(*this, key, permissive);
}

int64_t TraceEventFields::getInt64(std::string key, bool permissive) const {
	return getNumericValue<int64_t>(*this, key, permissive);
}

double TraceEventFields::getDouble(std::string key, bool permissive) const {
	return getNumericValue<double>(*this, key, permissive);
}

std::string TraceEventFields::toString() const {
	std::string str;
	bool first = true;
	for(auto itr = begin(); itr != end(); ++itr) {
		if(!first) {
			str += ", ";
		}
		first = false;

		str += format("\"%s\"=\"%s\"", itr->first.c_str(), itr->second.c_str());
	}

	return str;
}

bool validateField(const char *key, bool allowUnderscores) {
	if((key[0] < 'A' || key[0] > 'Z') && key[0] != '_') {
		return false;
	}

	const char* underscore = strchr(key, '_');
	while(underscore) {
		if(!allowUnderscores || ((underscore[1] < 'A' || underscore[1] > 'Z') && key[0] != '_' && key[0] != '\0')) {
			return false;
		}

		underscore = strchr(&underscore[1], '_');
	}

	return true;
}

void TraceEventFields::validateFormat() const {
	if(g_network && g_network->isSimulated()) {
		for(Field field : fields) {
			if(!validateField(field.first.c_str(), false)) {
				fprintf(stderr, "Trace event detail name `%s' is invalid in:\n\t%s\n", field.first.c_str(), toString().c_str());
			}
			if(field.first == "Type" && !validateField(field.second.c_str(), true)) {
				fprintf(stderr, "Trace event detail Type `%s' is invalid\n", field.second.c_str());
			}
		}
	}
}
