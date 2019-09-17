#!/bin/sh
set -e -x

# Find root directory and system type

ROOT=$(dirname $(dirname $(readlink -f $0)))
echo $ROOT
cd $ROOT

# Cmake release build

mkdir -p $ROOT/build/release/device-opcua-c
cd $ROOT/build/release/device-opcua-c
cmake -DCMAKE_EXPORT_COMPILE_COMMANDS=ON -DCMAKE_BUILD_TYPE=Release $ROOT/src
make 2>&1 | tee release.log


# Cmake debug build

mkdir -p $ROOT/build/debug/device-opcua-c
cd $ROOT/build/debug/device-opcua-c
cmake -DCMAKE_EXPORT_COMPILE_COMMANDS=ON -DDEV_OPCUA_BUILD_DEBUG=ON -DCMAKE_BUILD_TYPE=Debug $ROOT/src
make 2>&1 | tee debug.log

