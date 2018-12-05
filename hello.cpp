#include "flow/flow.h"

#include <iostream>
#include <string>
using namespace std;

#define RUN_TEST(FUNC) do { cout <<  "Running test " << #FUNC << endl; FUNC();\
                            cout << endl; } while (0)

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
