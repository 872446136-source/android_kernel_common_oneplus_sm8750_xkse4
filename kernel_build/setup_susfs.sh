#!/bin/bash

set -e

echo "==== SUSFS install ===="


if [ -d susfs ]; then
    rm -rf susfs
fi


git clone --depth=1 \
https://github.com/sidex15/susfs4ksu.git \
susfs


echo "SUSFS cloned"


find susfs -type f | head


echo "SUSFS source ready"
