#include <iostream>
#include "flow/flow.h"
#include "flow/actorcompiler.h"  // This must be the last #include

using namespace std;

ACTOR Future<Void> dummy() {
  state Future<Void> onChange = Void();

  loop choose {
    when (wait(onChange)) {
      cout << "onChange changed\n";
      break;
    }
  }
  return Void();
}