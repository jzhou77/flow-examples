# Flow Examples

Examples for using flow language developed in FoundationDB project. These 
examples are intended to learn the basics of the language.

## Compile

This assumes that you have put the directory under the foundationdb folder and
you have compiled foundationdb using the provided docker image, i.e.,
"build/Dockerfile".

To compile, enter the docker image

```bash
$ docker run --name DOCKERCONTAINER-36563  -v /Users/$USER/fdb/fdb-tools/scripts/../..:/opt/foundation:delegated -e FDB_TLS_PLUGIN=/opt/foundation/foundationdb/tls-plugins/FDBGnuTLS.so -e LOGNAME=$USER -e CCACHE_DIR=/opt/foundation/.ccache -e PYTHONPATH=/opt/foundation/foundationdb/bindings/python -e GOPATH=/opt/go -e LD_LIBRARY_PATH=/opt/foundation/foundationdb/lib -e LIBRARY_PATH=/usr/local/lib -e TLS_LIBDIR=/usr/local/lib -w /opt/foundation -u root --rm --init --privileged -it -e CC=/opt/x-toolchain/bin/x86_64-nptl-linux-gnu-gcc -e CXX=/opt/x-toolchain/bin/x86_64-nptl-linux-gnu-g++ docker.apple.com/cie_fdb/foundationdb-dev:0.8.7 bash
root@fce4696cc1b9:/opt/foundation# cd foundationdb/flow-examples/
root@fce4696cc1b9:/opt/foundation/foundationdb/flow-examples# make
```

Compilation on other platforms have not been attempted.

### Compilation Problems


```bash
root@fce4696cc1b9:/opt/foundation/foundationdb/examples# make
/opt/x-toolchain/bin/x86_64-nptl-linux-gnu-g++ -std=c++0x -I/opt/boost_1_52_0 -I/opt/foundation/foundationdb hello.o /opt/foundation/foundationdb/lib/libflow.a /opt/foundation/foundationdb/lib/libfdb_flow.a -Bstatic -ldl -lpthread -lrt -lfdb_c -Bdynamic  /opt/x-toolchain/x86_64-nptl-linux-gnu/lib64/libstdc++.a -lm -o hello -L/usr/local/lib -L/opt/foundation/foundationdb/lib -L/opt/x-toolchain/x86_64-nptl-linux-gnu/lib64/
hello.o: In function `returnIfTrue(Future<bool>)':
hello.cpp:(.text+0x134): undefined reference to `bool wait<bool>(Future<bool> const&)'
hello.cpp:(.text+0x15e): undefined reference to `wait(Never const&)'
hello.o: In function `shortCircuitAny(std::vector<Future<bool>, std::allocator<Future<bool> > >)':
hello.cpp:(.text+0x2e6): undefined reference to `Void wait<Void>(Future<Void> const&)'
hello.o: In function `Future<Void> tagError<Void>(Future<Void>, Error)':
hello.cpp:(.text._Z8tagErrorI4VoidE6FutureIT_ES1_IS0_E5Error[_Z8tagErrorI4VoidE6FutureIT_ES1_IS0_E5Error]+0x1b): undefined reference to `Void wait<Void>(Future<Void> const&)'
collect2: error: ld returned 1 exit status
Makefile:35: recipe for target 'hello' failed
make: *** [hello] Error 1
```

Seems this can be solved by putting a modified "flow" directory under "examples". Not sure why?

TODO: add modified "flow"?

## Examples

### hello.cpp

```c++
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
```

The output for the above code is:

```shell
# ./hello
Running test hello
Hello, World!

Running test hello2
Before send: promise isSet = 0, future.isReady = 0
After send: promise isSet = 1, future.isReady = 1
Hello, World!

```

In hellow2(), before sending the value, the future "f" is not ready. "f" only
becomes ready after Promise sends the value.

### calc.cpp & calc.actor.cpp

The ACTOR waits for "f", then does the computation, and finally returns a future.

```c++
ACTOR Future<int> asyncAdd(Future<int> f, int offset) {
    int value = wait( f );
    return value + offset;
}
```

The main function in calc.cpp shows that we can have the future of this asyncAdd,
which only becomes available after "p" sends a value to "f".

```c++
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
```
