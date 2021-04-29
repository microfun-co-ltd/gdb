#!/bin/bash


if [ -z "$1" ]
then
  echo Usage:
  echo '  '${0##*/} target_arch api_level [make_target [make_options...]]
  echo
  echo E.g.:
  echo '  '${0##*/} arm64-v8a 21
  echo '  '${0##*/} armeabi-v7a 14
  echo '  '${0##*/} x86_64 21
  echo '  '${0##*/} x86 14
  exit 1
fi

TARGET_ARCH=$1
API_LEVEL=$2
BUILD_ACTION=${3-all}

shift
shift
shift

echo Building for architecture $TARGET_ARCH and Android API level $API_LEVEL

NDK_DIR=`grep ndk.dir local.properties | cut -f 2 -d =`
if [ -z "$NDK_DIR" ]
then
  echo Cannot find ndk.dir property in local.properties file
  exit 2
fi

echo Using Android NDK at: $NDK_DIR

case "$TARGET_ARCH" in
  arm64-v8a)
    TOOLCHAIN_DIR=aarch64-linux-android-4.9
    BUILD_ARCH=aarch64-linux-android
    NDK_LIB_DIR=arch-arm64
    GCC_ARCH=
    EXTRA_CFG=
    ;;

  armeabi-v7a)
    TOOLCHAIN_DIR=arm-linux-androideabi-4.9
    BUILD_ARCH=arm-linux-androideabi
    NDK_LIB_DIR=arch-arm
    GCC_ARCH=-march=armv7-a
    EXTRA_CFG=--disable-largefile
    ;;

  x86)
    TOOLCHAIN_DIR=x86-4.9
    BUILD_ARCH=i686-linux-android
    NDK_LIB_DIR=arch-x86
    GCC_ARCH=
    EXTRA_CFG=
    ;;

  x86_64)
    TOOLCHAIN_DIR=x86_64-4.9
    BUILD_ARCH=x86_64-linux-android
    NDK_LIB_DIR=arch-x86_64
    GCC_ARCH=
    EXTRA_CFG=
    ;;
esac


#
# Prepare environment
#

export PATH=${NDK_DIR}/toolchains/${TOOLCHAIN_DIR}/prebuilt/linux-x86_64/${BUILD_ARCH}/bin:${NDK_DIR}/toolchains/${TOOLCHAIN_DIR}/prebuilt/linux-x86_64/bin:$PATH

export LDFLAGS="-pie -Wl,--gc-sections --sysroot=${NDK_DIR}/platforms/android-${API_LEVEL}/${NDK_LIB_DIR} -L${NDK_DIR}/platforms/android-${API_LEVEL}/${NDK_LIB_DIR}/usr/lib -L${NDK_DIR}/platforms/android-${API_LEVEL}/${NDK_LIB_DIR}/usr/lib -L${NDK_DIR}/toolchains/${TOOLCHAIN_DIR}/prebuilt/linux-x86_64/lib/gcc/${BUILD_ARCH}/4.9.x -L${NDK_DIR}/sources/cxx-stl/gnu-libstdc++/4.9/libs/${TARGET_ARCH} -Lzstd/build/cmake/build_${TARGET_ARCH}/lib"
export CFLAGS="${GCC_ARCH} -D__ANDROID_API__=${API_LEVEL} -g -DANDROID -Os -fdata-sections -ffunction-sections -Wl,--gc-sections -fPIE --sysroot=${NDK_DIR}/sysroot -I${NDK_DIR}/sysroot/usr/include/${BUILD_ARCH} -I${NDK_DIR}/sysroot/usr/include -I${NDK_DIR}/sources/cxx-stl/gnu-libstdc++/4.9/include -I${NDK_DIR}/sources/cxx-stl/gnu-libstdc++/4.9/libs/${TARGET_ARCH}/include -Izstd/lib"
export CXXFLAGS="-std=c++11 $CFLAGS"
export CPPFLAGS="$CFLAGS"

rm -fr build_${TARGET_ARCH}
mkdir build_${TARGET_ARCH}
cd build_${TARGET_ARCH}


#
# Configure the project
#

../configure --host=${BUILD_ARCH} --target=${BUILD_ARCH} \
  --disable-source-highlight \
  --with-system-zlib \
  --enable-compressed-debug-sections=none \
  ${EXTRA_CFG} \
  --disable-libquadmath-support \
  --disable-python \
  --disable-go-lang \
  --disable-doc \
  --without-isl \
  --without-python \
  --without-go-lang \
  --with-build-sysroot=${NDK_DIR}/sysroot \
  --with-sysroot=${NDK_DIR}/sysroot

#
# Build
#

#`cat /proc/cpuinfo | grep ^processor | wc -l`
make -j4 V=1 \
  CFLAGS="$CFLAGS" \
  CXXFLAGS="$CXXFLAGS" \
  LDFLAGS="$LDFLAGS" \
  XM_CLIBS="-lzstd -lsupc++ -lgnustl_static -lstdc++ -lgcc -lc" \
  $* | tee build.log


cp gdb/gdb gdb/gdb.sym

${NDK_DIR}/toolchains/${TOOLCHAIN_DIR}/prebuilt/linux-x86_64/${BUILD_ARCH}/bin/strip gdb/gdb
