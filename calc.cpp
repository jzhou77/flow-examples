#include "flow/flow.h"

#include <iostream>
#include <string>
using namespace std;

// Note the "const &" usage here, even though the calc.actor.cpp doesn't use
// "cost &". This modifier is added by actor compiler in the generated
// calc.actor.g.cpp file.
Future<int> asyncAdd(Future<int> const& f, int const& offset);

int main(int argc, char** argv) {
  Promise<int> p;
  Future<int> f = p.getFuture();
  Future<int> result = asyncAdd(f, 10);
  cout << "Future f.isReady = " << f.isReady() << ", result.isReady = " 
       << result.isReady() << endl;
  p.send( 5 );
  cout << "Send 5 to f" << endl;
  cout << "Result is " << result.get() << endl; // f is already set

  return 0;
}