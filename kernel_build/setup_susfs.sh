#!/bin/bash

set -e

echo "==== SUSFS install ===="

git clone --depth=1 \
https://github.com/sidex15/susfs4ksu-module.git \
susfs

cp -r susfs/kernel/* .

echo "SUSFS done"
