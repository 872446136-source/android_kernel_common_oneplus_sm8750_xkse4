#!/bin/bash

set -e

echo "==== ReSukiSU install ===="

if [ -d KernelSU ]; then
    rm -rf KernelSU
fi

git clone --depth=1 \
https://github.com/5ec1cff/KernelSU.git \
KernelSU

cd KernelSU

bash setup.sh

cd ..

echo "ReSukiSU done"
