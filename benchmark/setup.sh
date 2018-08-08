#!/bin/bash

# assumes you're using "build" and "install" directories.
# run relative to your atJIT build dir.

git clone --depth=1 https://github.com/google/benchmark.git
git clone --depth=1 https://github.com/google/googletest.git benchmark/googletest
mkdir benchmark/build
pushd benchmark/build
cmake .. -GNinja -DCMAKE_INSTALL_PREFIX=`pwd`/../install
ninja
ninja install
popd