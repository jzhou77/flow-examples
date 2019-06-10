/*
 * serialize.cpp
 *
 * This source file is part of the FoundationDB open source project
 *
 * Copyright 2013-2019 Apple Inc. and the FoundationDB project authors
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

#include "flow/serialize.h"
#include "flow/network.h"

_AssumeVersion::_AssumeVersion( uint64_t version ) : v(version) {
	if( version < minValidProtocolVersion ) {
		ASSERT(!g_network->isSimulated());
		throw serialization_failed();
	}
}

const void* BinaryReader::readBytes( int bytes ) {
	const char* b = begin;
	const char* e = b + bytes;
	if( e > end ) {
		ASSERT(!g_network->isSimulated());
		throw serialization_failed();
	}
	begin = e;
	return b;
}