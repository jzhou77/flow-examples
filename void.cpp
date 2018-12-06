#include <iostream>
#include <string>
#include "flow/flow.h"
#include "test.h"

using namespace std;

Future<Void> dummy();

void test1() {
  Future<Void> f = Void(), n = Never();
  cout << "f = Void(), f.isReady() = " << f.isReady() << endl;
  cout << "n = Never(), n.isReady() = " << f.isReady() << endl;
}

void test2() {
  Future<Void> result = dummy();
  cout << "f = Void(), f.isReady() = " << result.isReady() << endl;
}

int main(int argc, char **argv) {
  RUN_TEST(test1);
  RUN_TEST(test2);

  return 0;
}
