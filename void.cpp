#include <iostream>
#include <string>
#include "flow/flow.h"
#include "test.h"

using namespace std;

Future<Void> dummy();
Future<Void> foo();
Future<Void> never();
Future<Void> never2(int const& i);

void test1() {
  Future<Void> f = Void(), n = Never();
  cout << "f = Void(), f.isReady() = " << f.isReady() << endl;
  cout << "n = Never(), n.isReady() = " << f.isReady() << endl;
}

void test2() {
  Future<Void> result = dummy();
  cout << "result = dummy(), result.isReady() = " << result.isReady() << endl;
}

void test3() {
  Future<Void> result = foo();
  cout << "result = foo(), result.isReady() = " << result.isReady() << endl;
}

void test4() {
  Future<Void> result = never();
  cout << "result = never(), result.isReady() = " << result.isReady() << endl;
}

void test5() {
  const int not_used = 1;
  Future<Void> result = never2(not_used);
  cout << "result = never2(), result.isReady() = " << result.isReady() << endl;
}

int main(int argc, char **argv) {
  RUN_TEST(test1);
  RUN_TEST(test2);
  RUN_TEST(test3);
  RUN_TEST(test4);
  RUN_TEST(test5);

  return 0;
}
