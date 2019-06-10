/*
 * flow.cpp
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
#include <stdarg.h>

INetwork *g_network = 0;
IRandom *g_random = 0;
IRandom *g_nondeterministic_random = 0;
IRandom *g_debug_random = 0;
FILE* randLog = 0;
uint64_t debug_lastLoadBalanceResultEndpointToken = 0;
bool noUnseed = false;

std::string UID::toString() const {
	return format("%016llx%016llx", part[0], part[1]);
}

UID UID::fromString( std::string const& s ) {
	ASSERT( s.size() == 32 );
	uint64_t a=0, b=0;
	int r = sscanf( s.c_str(), "%16llx%16llx", &a, &b );
	ASSERT( r == 2 );
	return UID(a, b);
}

std::string UID::shortString() const {
	return format("%016llx", part[0]);
}

void detectFailureAfter( int const& address, double const& delay );

Optional<uint64_t> parse_with_suffix(std::string toparse, std::string default_unit) {
	char *endptr;

	uint64_t ret = strtoull(toparse.c_str(), &endptr, 10);

	if (endptr == toparse.c_str()) {
		return Optional<uint64_t>();
	}

	std::string unit;

	if (*endptr == '\0') {
		if (!default_unit.empty()) {
			unit = default_unit;
		} else {
			return Optional<uint64_t>();
		}
	} else {
		unit = endptr;
	}

	if (!unit.compare("B")) {
		// Nothing to do
	} else if (!unit.compare("KB")) {
		ret *= int64_t(1e3);
	} else if (!unit.compare("KiB")) {
		ret *= 1LL << 10;
	} else if (!unit.compare("MB")) {
		ret *= int64_t(1e6);
	} else if (!unit.compare("MiB")) {
		ret *= 1LL << 20;
	} else if (!unit.compare("GB")) {
		ret *= int64_t(1e9);
	} else if (!unit.compare("GiB")) {
		ret *= 1LL << 30;
	} else if (!unit.compare("TB")) {
		ret *= int64_t(1e12);
	} else if (!unit.compare("TiB")) {
		ret *= 1LL << 40;
	} else {
		return Optional<uint64_t>();
	}

	return ret;
}

int vsformat( std::string &outputString, const char* form, va_list args) {
	char buf[200];

	va_list args2;
	va_copy(args2, args);
	int size = vsnprintf(buf, sizeof(buf), form, args2);
	va_end(args2);

	if(size >= 0 && size < sizeof(buf)) {
		outputString = std::string(buf, size);
		return size;
	}

	#ifdef _WIN32
	// Microsoft's non-standard vsnprintf doesn't return a correct size, but just an error, so determine the necessary size
	va_copy(args2, args);
	size = _vscprintf(form, args2);
	va_end(args2);
	#endif

	if (size < 0) {
		return -1;
	}

	TEST(true); //large format result

	outputString.resize(size + 1);
	size = vsnprintf(&outputString[0], outputString.size(), form, args);
	if (size < 0 || size >= outputString.size()) {
		return -1;
	}

	outputString.resize(size);
	return size;
}

std::string format( const char* form, ... ) {
	va_list args;
	va_start(args, form);

	std::string str;
	int result = vsformat(str, form, args);
	va_end(args);

	ASSERT(result >= 0);
	return str;
}

Standalone<StringRef> strinc(StringRef const& str) {
	int index;
	for(index = str.size() - 1; index >= 0; index--)
		if(str[index] != 255)
			break;

	// Must not be called with a string that consists only of zero or more '\xff' bytes.
	ASSERT(index >= 0);

	Standalone<StringRef> r = str.substr(0, index+1);
	uint8_t *p = mutateString(r);
	p[r.size()-1]++;
	return r;
}

StringRef strinc(StringRef const& str, Arena& arena) {
	int index;
	for(index = str.size() - 1; index >= 0; index--)
		if(str[index] != 255)
			break;

	// Must not be called with a string that consists only of zero or more '\xff' bytes.
	ASSERT(index >= 0);

	StringRef r( arena, str.substr(0, index+1) );
	uint8_t *p = mutateString(r);
	p[r.size()-1]++;
	return r;
}

StringRef addVersionStampAtEnd(StringRef const& str, Arena& arena) {
	int32_t size = str.size();
	uint8_t* s = new (arena) uint8_t[size + 14];
	memcpy(s, str.begin(), size);
	memset(&s[size], 0, 10);
	memcpy(&s[size+10], &size, 4);
	return StringRef(s,size + 14);
}

Standalone<StringRef> addVersionStampAtEnd(StringRef const& str) {
	Standalone<StringRef> r;
	((StringRef &)r) = addVersionStampAtEnd(str, r.arena());
	return r;
}

bool buggifyActivated = false;
std::map<std::pair<std::string,int>, int> SBVars;

double P_BUGGIFIED_SECTION_ACTIVATED = .25,
	P_BUGGIFIED_SECTION_FIRES = .25,
	P_EXPENSIVE_VALIDATION = .05;

int getSBVar(std::string file, int line){
	if (!buggifyActivated) return 0;

	const auto &flPair = std::make_pair(file, line);
	if (!SBVars.count(flPair)){
		SBVars[flPair] = g_random->random01() < P_BUGGIFIED_SECTION_ACTIVATED;
		g_traceBatch.addBuggify( SBVars[flPair], line, file );
		if( g_network ) g_traceBatch.dump();
	}

	return SBVars[flPair];
}

bool validationIsEnabled() {
	return buggifyActivated;
}

void enableBuggify( bool enabled ) {
	buggifyActivated = enabled;
}