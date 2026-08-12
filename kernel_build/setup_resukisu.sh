#!/bin/bash

set -e

echo "==== ReSukiSU install ===="


if [ -d KernelSU ]; then
    rm -rf KernelSU
fi


git clone --depth=1 \
https://github.com/rsuntk/KernelSU.git \
KernelSU


echo "KernelSU cloned"


echo "Applying KernelSU"


cp -r KernelSU/* .


echo "ReSukiSU setup done"
