#include <iostream>
#include <string>
#include "flow/Trace.h"
#include "flow/DeterministicRandom.h"
#include "flow/flow.h"
#include "test.h"

using namespace std;

void delayTest();

int main(int argc, char **argv) {
  int randomSeed = platform::getRandomSeed();
  g_random = new DeterministicRandom(randomSeed);
  g_nondeterministic_random = new DeterministicRandom(platform::getRandomSeed());
  g_network = newNet2( NetworkAddress(), false );

  RUN_TEST(delayTest);
  cout << "delayTest running...\n";
  g_network->run();
  cout << "delayTest existing...\n";

  return 0;
}
