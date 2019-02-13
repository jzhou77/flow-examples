#include <iostream>
#include "flow/flow.h"
#include "flow/actorcompiler.h"  // This must be the last #include

using namespace std;

ACTOR Future<Void> infinite_loop() {
  state Future<Void> onChange = Void();
  state int count = 0;

  loop choose {
    when( wait( delay(0.01) ) ) { break; }
    when( wait( onChange ) ) {
      // onChange = Never();
      count++;
      if (count % 1000 == 0) {
        std::cout << "Loop count " << count << std::endl;
      }
    }
  }
  cout << "loop returned.\n";
  return Void();
}

ACTOR Future<Void> loopTest() {
  wait( infinite_loop() );
  cout << "ACTOR loopTest done...\n" << endl;
  g_network->stop();
  return Void();
}