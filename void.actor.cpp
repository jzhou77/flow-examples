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
  cout << "foo returned.\n";
  return Void();
}

ACTOR Future<Void> never() {
  wait( Future<Void>(Never()) );
  // Not reached, because wiat() never returns.
  cout << "never returned.\n";
  return Void();
}

ACTOR Future<Void> never2(int select) {
  loop {
    state Future<Void> reg = Never();

    choose {
      when( wait( reg )) { break; }
    }
  }
  // Not reached, because "reg" never breaks from the loop
  cout << "never2 returned.\n";
  return Void();
}
