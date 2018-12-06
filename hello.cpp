#include <iostream>
#include <string>
#include "test.h"
#include "flow/flow.h"

using namespace std;

void hello() {
  Promise<string> p;
  Future<string> f = p.getFuture();
  p.send( "Hello, World!" );
  cout<< f.get() << endl; // f is already set
}

void hello2() {
  Promise<string> p;
  Future<string> f = p.getFuture();
  cout << "Before send: promise isSet = " << p.isSet() << ", future.isReady = "
       << f.isReady() << endl;
  p.send( "Hello, World!" );
  cout << "After send: promise isSet = " << p.isSet() << ", future.isReady = "
       << f.isReady() << endl;
  cout<< f.get() << endl; // f is already set
}

int main(int argc, char **argv) {
  RUN_TEST(hello);
  RUN_TEST(hello2);
  return 0;
}
