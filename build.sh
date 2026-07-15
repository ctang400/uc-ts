#!/bin/bash
cd "${0%/*}"
BUILD_ROOT=build
SCRIPT_DIR=$( cd -- "$( dirname -- "${BASH_SOURCE[0]}" )" &> /dev/null && pwd )

echo $BUILD_ROOT

conan install $SCRIPT_DIR/conanfile.txt --build missing -if $BUILD_ROOT

cmake -B ${BUILD_ROOT} $SCRIPT_DIR
cmake --build  ${BUILD_ROOT} -j
echo '*' > ${BUILD_ROOT}/.gitignore
