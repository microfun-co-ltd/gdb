#!/bin/bash

if [ -z "$1" ]
then
  echo Usage:
  echo '  '${0##*/} target_arch api_level [make_target [make_options...]]
  echo
  echo E.g.:
  echo '  '${0##*/} arm64-v8a 21 Debug
  echo '  '${0##*/} armeabi-v7a 14 Release
  echo '  '${0##*/} x86_64 21 Release
  echo '  '${0##*/} x86 14 Debug
  exit 1
fi

TARGET_ARCH=$1
API_LEVEL=$2
BUILD_TYPE=${3-Debug}

NDK_DIR=`grep ndk.dir local.properties | cut -f 2 -d =`
if [ -z "$NDK_DIR" ]
then
  echo Cannot find ndk.dir property in local.properties file
  exit 2
fi

CMAKE_DIR=`grep cmake.dir local.properties | cut -f 2 -d =`
if [ -z "$CMAKE_DIR" ]
then
  echo Cannot find cmake.dir property in local.properties file
  exit 2
fi


${CMAKE_DIR}/bin/cmake -H. \
  -Bbuild_${TARGET_ARCH} \
  -DCMAKE_LIBRARY_OUTPUT_DIRECTORY=.. \
  -DANDROID_ABI=${TARGET_ARCH} \
  -DANDROID_PLATFORM=android-${API_LEVEL} \
  -DCMAKE_BUILD_TYPE=${BUILD_TYPE} \
  -DANDROID_NDK=${NDK_DIR} \
  -DCMAKE_SYSTEM_NAME=Android \
  -DCMAKE_ANDROID_ARCH_ABI=${TARGET_ARCH} \
  -DCMAKE_SYSTEM_VERSION=${API_LEVEL} \
  -DCMAKE_EXPORT_COMPILE_COMMANDS=ON \
  -DCMAKE_ANDROID_NDK=${NDK_DIR} \
  -DANDROID_TOOLCHAIN=clang \
  -DCMAKE_TOOLCHAIN_FILE=${NDK_DIR}/build/cmake/android.toolchain.cmake \
  -G Ninja \
  -DCMAKE_MAKE_PROGRAM=${CMAKE_DIR}/bin/ninja

${CMAKE_DIR}/bin/cmake --build build_${TARGET_ARCH}

