#!/bin/bash

# linux gcc arm64 native build

CMAKE_BIN=cmake
BUILD_DIR_NAME=build-gcc-aarch64

rm -rf ${BUILD_DIR_NAME}
mkdir ${BUILD_DIR_NAME}

cd ${BUILD_DIR_NAME}

$CMAKE_BIN \
  -DCMAKE_BUILD_TYPE=RelWithDebInfo \
  -DEMBREE_ARM=On \
  -DCMAKE_INSTALL_PREFIX=$HOME/local/embree3 \
  -DCMAKE_C_COMPILER=gcc \
  -DCMAKE_CXX_COMPILER=g++ \
  -DCMAKE_CXX_FLAGS=-flax-vector-conversions\
  -DEMBREE_IGNORE_CMAKE_CXX_FLAGS=OFF \
  -DEMBREE_ISPC_SUPPORT=Off \
  -DEMBREE_TASKING_SYSTEM=Internal \
  -DEMBREE_TUTORIALS=Off \
  -DEMBREE_RAY_PACKETS=Off \
  -DEMBREE_MAX_ISA=AVX2 \
  -DEMBREE_NEON_AVX2_EMULATION=ON \
  ..

cd ..