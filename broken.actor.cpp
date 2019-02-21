#include <iostream>
#include "flow/Error.h"
#include "flow/flow.h"
#include "flow/actorcompiler.h"  // This must be the last #include

using namespace std;

ACTOR Future<int> promise_broken(Future<int>* f) {
  state Promise<int> p;

  *f = p.getFuture();
  wait(delay(0.1));
  // Exiting without sending value results in broken promise.
  // p.send(1);
  return 2;
}

ACTOR Future<Void> brokenTest() {
  try {
    state Future<int> s;
    state Future<int> f = promise_broken(&s);
    loop choose {
      when (int v = wait(f)) {
        cout << "Got value from function " << v << endl;
        f = Never();
      }
      when (int v = wait(s)) {
        cout << "Got value from promise " << v << endl;
        s = Never();
      }
    }
  } catch (Error& err) {
    cout << "Error: " << err.name() << endl;
  }
  g_network->stop();
  return Void();
}