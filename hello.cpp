#include "flow/flow.h"

#include <iostream>
#include <string>
using namespace std;

int main(int argc, char **argv) {
  Promise<string> p;
  Future<string> f = p.getFuture();
  p.send( "Hello, World!" );
  cout<< f.get() << endl; // f is already set
}
