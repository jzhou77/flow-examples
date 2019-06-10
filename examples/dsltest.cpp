#include <iostream>
#include <string>
#include "flow/flow.h"
#include "test.h"

using namespace std;

void dsltest();

int main(int argc, char **argv) {
  RUN_TEST(dsltest);

  return 0;
}
