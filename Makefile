PLATFORM := $(shell uname)

ifeq ($(PLATFORM),Linux)
# set for docker image
CC = /opt/x-toolchain/bin/x86_64-nptl-linux-gnu-gcc
CXX=/opt/x-toolchain/bin/x86_64-nptl-linux-gnu-g++
MONO=/usr/bin/mono
BOOSTDIR=/opt/boost_1_52_0
FDB=/opt/foundation/foundationdb
ACTORCOMPILER=$(FDB)/bin/actorcompiler.exe

CFLAGS += 
CXXFLAGS += -std=c++0x -g
INCLUDE = -I$(BOOSTDIR) -I$(FDB)
LFLAGS = $(FDB)/lib/libflow.a $(FDB)/lib/libfdb_flow.a -Bstatic -ldl -lpthread -lrt -lfdb_c -Bdynamic  /opt/x-toolchain/x86_64-nptl-linux-gnu/lib64/libstdc++.a -lm
LDFLAGS = -L/usr/local/lib -L$(FDB)/lib -L/opt/x-toolchain/x86_64-nptl-linux-gnu/lib64/


else ifeq ($(PLATFORM),Darwin)
CC = /usr/bin/clang
CXX = /usr/bin/clang++

CFLAGS +=  -mmacosx-version-min=10.7 -stdlib=libc++
LDFLAGS := -lflow -framework IOKit -framework CoreFoundation
LFLAGS += -Lflow -F /Applications/Xcode.app/Contents/Developer/Platforms/MacOSX.platform/Developer/SDKs/MacOSX10.14.sdk/System/Library/Frameworks/IOKit.framework/

INCLUDE = -I. -Iboost_1_52_0
CXXFLAGS += -g -std=c++11 -msse4.2 -Wno-undefined-var-template -Wno-unknown-warning-option
endif

TARGETS = hello calc void

.cpp.o:
	$(CXX) $(CXXFLAGS) $(INCLUDE) -c $< -o $@

%.actor.g.cpp: %.actor.cpp $(ACTORCOMPILER)
	@echo "Actorcompiling $<"
	@$(MONO) $(ACTORCOMPILER) $< $@
.PRECIOUS: %.actor.g.cpp

all: $(TARGETS)

hello: hello.o
	$(CXX) $(CXXFLAGS) $(INCLUDE) $< $(LFLAGS) -o $@ $(LDFLAGS)

calc: calc.o calc.actor.g.o
	$(CXX) $(CXXFLAGS) $^ -o $@ $(LFLAGS) $(LDFLAGS)

void: void.o void.actor.g.o
	$(CXX) $(CXXFLAGS) $(INCLUDE) $^ $(LFLAGS) -o $@ $(LDFLAGS)

clean:
	@rm *.o $(TARGETS)

.PHONY: all clean