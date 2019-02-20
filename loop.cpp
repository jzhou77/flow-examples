#include <iostream>
#include <string>
#include "flow/Trace.h"
#include "flow/DeterministicRandom.h"
#include "flow/flow.h"
#include "test.h"

using namespace std;

void loopTest();
void delayTest();

void usage(const char* program) {
  cout << "Usage: " << program << " loop|delay" << endl;
}

int main(int argc, char **argv) {
  int randomSeed = platform::getRandomSeed();
  g_random = new DeterministicRandom(randomSeed);
  g_nondeterministic_random = new DeterministicRandom(platform::getRandomSeed());
  g_network = newNet2(false);

  if (argc != 2) {
    usage(argv[0]);
    return 0;
  }

  if (!strcmp(argv[1], "loop")) {
    RUN_TEST(loopTest);
    cout << argv[1] << "Test running... (expecting 2s delay)\n";
  } else if (!strcmp(argv[1], "delay")) {
    RUN_TEST(delayTest);
    cout << argv[1] << "Test running... (expecting 5s delay)\n";
  } else {
    usage(argv[0]);
    return -1;
  }
  g_network->run();
  cout << argv[1] << "Test existing...\n";

  return 0;
}
