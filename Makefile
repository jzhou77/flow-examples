PLATFORM := $(shell uname)

ifeq ($(PLATFORM),Linux)
# set for docker image
CC = /opt/x-toolchain/bin/x86_64-nptl-linux-gnu-gcc
CXX=/opt/x-toolchain/bin/x86_64-nptl-linux-gnu-g++
BOOSTDIR=/opt/boost_1_52_0
FDB=/opt/foundation/foundationdb

CFLAGS += 
CXXFLAGS += -std=c++0x
INCLUDE = -I$(BOOSTDIR) -I$(FDB)
LFLAGS = /opt/foundation/foundationdb/lib/libflow.a /opt/foundation/foundationdb/lib/libfdb_flow.a -Bstatic -ldl -lpthread -lrt -lfdb_c -Bdynamic  /opt/x-toolchain/x86_64-nptl-linux-gnu/lib64/libstdc++.a -lm
LDFLAGS = -L/usr/local/lib -L$(FDB)/lib -L/opt/x-toolchain/x86_64-nptl-linux-gnu/lib64/


else ifeq ($(PLATFORM),Darwin)
CC = /usr/bin/clang
CXX = /usr/bin/clang++

CFLAGS +=  -mmacosx-version-min=10.7 -stdlib=libc++
LDFLAGS := -lflow -framework IOKit -framework CoreFoundation
LFLAGS += -Lflow -F /Applications/Xcode.app/Contents/Developer/Platforms/MacOSX.platform/Developer/SDKs/MacOSX10.14.sdk/System/Library/Frameworks/IOKit.framework/

INCLUDE = -I. -Iboost_1_52_0
CXXFLAGS += -std=c++11 -msse4.2 -Wno-undefined-var-template -Wno-unknown-warning-option
endif

TARGETS = hello

.cpp.o:
	$(CXX) $(CXXFLAGS) $(INCLUDE) -c $< -o $@

hello: hello.o
	$(CXX) $(CXXFLAGS) $(INCLUDE) $< $(LFLAGS) -o $@ $(LDFLAGS)

all: $(TARGETS)
