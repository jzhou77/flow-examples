#include "flow/flow.h"

#include "flow/actorcompiler.h"  // This must be the last #include

ACTOR Future<int> asyncAdd(Future<int> f, int offset) {
    int value = wait( f );
    return value + offset;
}