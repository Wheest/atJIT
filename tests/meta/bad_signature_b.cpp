// RUN: %not %easycc  -O2  %s -o %t 2> %t.log
// RUN: %FileCheck %s < %t.log

#include <easy/jit.h>
#include <easy/options.h>

using namespace std::placeholders;

int foo(int, int, int) {
  return 0;
}

int main(int, char** argv) {

  auto foo_ = easy::jit(foo, 1, 2);
  foo_(); // CHECK: easy::jit: not providing enough argument to actual call
  return 0;
}
