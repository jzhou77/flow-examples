#include <iostream>
#include "flow/flow.h"
#include "flow/actorcompiler.h"  // This must be the last #include

using namespace std;

ACTOR Future<Void> delay_five() {
  state Future<Void> reg = Never();
  state Future<Void> onChange = Void();
  loop choose {
    when( wait( reg )) { break; }
    when( wait( onChange ) ) {
      wait( delay(5) );
      break;
    }    
  }
  cout << "delay_five returned.\n";
  return Void();
}

ACTOR void delayTest() {
  wait( delay_five() );
  cout << "ACTOR delayTest done...\n" << endl;
  g_network->stop();
}