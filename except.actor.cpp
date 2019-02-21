#include <iostream>
#include "flow/Error.h"
#include "flow/error_definitions.h"
#include "flow/flow.h"
#include "flow/actorcompiler.h"  // This must be the last #include

using namespace std;

ACTOR Future<int> raise_exception() {
  wait(delay(0.1));
  cout << "Throw exception in " << __FUNCTION__ << endl;
  throw value_too_large();
}

ACTOR Future<Void> exceptTest() {
  try {
    state Future<int> s = raise_exception();
    state Future<Void> f = delay(1.0);
    loop choose {
      when (wait(f)) {
        break;
      }
      // No wait means no exceptions caught.
      // when (int i = wait(s)) {}
    }
  } catch (Error& err) {
    cout << "Caught error: " << err.name() << endl;
  }
  g_network->stop();
  return Void();
}