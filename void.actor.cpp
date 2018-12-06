#include <iostream>
#include "flow/flow.h"
#include "flow/actorcompiler.h"  // This must be the last #include

using namespace std;

ACTOR Future<Void> dummy() {
  state Future<Void> onChange = Void();

  loop choose {
    when (wait(onChange)) {
      cout << "dummy onChange changed\n";
      break;
    }
  }
  return Void();
}

ACTOR Future<Void> foo() {
  state Future<Void> onChange = dummy();

  loop choose {
    when (wait(onChange)) {
      cout << "foo onChange changed\n";
      break;
    }
  }
  return Void();
}

ACTOR Future<Void> never() {
  wait( Future<Void>(Never()) );
  return Void();
}

ACTOR Future<Void> never2(int select) {
  loop {
    state Future<Void> reg = Never();

    choose {
      when( wait( reg )) { break; }
    }
  }
  return Void();
}
