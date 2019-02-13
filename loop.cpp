#include <iostream>
#include <string>
#include "flow/Trace.h"
#include "flow/DeterministicRandom.h"
#include "flow/flow.h"
#include "test.h"

using namespace std;

void loopTest();

int main(int argc, char **argv) {
  int randomSeed = platform::getRandomSeed();
  g_random = new DeterministicRandom(randomSeed);
  g_nondeterministic_random = new DeterministicRandom(platform::getRandomSeed());
  g_network = newNet2( NetworkAddress(), false );

  RUN_TEST(loopTest);
  cout << "loopTest running... (expecting 2s delay)\n";
  g_network->run();
  cout << "loopTest existing...\n";

  return 0;
}
