#!/bin/bash

set -e

export BUILD_CONFIG=build.config.gki.aarch64
export KERNEL_LOCALVERSION=-oppo6.6

echo "=============================="
echo " OnePlus13 OPPO6.6 Build"
echo " SM8750 GKI 6.6"
echo " ReSukiSU + SUSFS"
echo " KPM OFF"
echo "=============================="


echo "[+] Prepare scripts"

chmod +x kernel_build/*.sh


echo "[+] Setup ReSukiSU"

kernel_build/setup_resukisu.sh


echo "[+] Setup SUSFS"

kernel_build/setup_susfs.sh


echo "[+] Apply config"

if [ -f configs/resukisu.fragment ]; then

    cat configs/resukisu.fragment \
    >> arch/arm64/configs/xkse4_dodge_defconfig

else

    echo "No resukisu.fragment found"

fi


echo "[+] Check build system"

ls -la

echo "==== check build directory ===="

ls -la build || true


echo "[+] Build Kernel"


if command -v bazel >/dev/null 2>&1; then

    echo "Using system bazel"

    bazel build \
    --config=fast \
    --config=gki \
    //common:kernel_aarch64


elif [ -f tools/bazel ]; then

    echo "Using tools/bazel"

    tools/bazel build \
    --config=fast \
    --config=gki \
    //common:kernel_aarch64


else

    echo "Bazel not found"

    echo "Try Make build"

    make \
    O=out \
    ARCH=arm64 \
    xkse4_dodge_defconfig


    make \
    O=out \
    ARCH=arm64 \
    LLVM=1 \
    -j$(nproc)

fi


echo "=============================="
echo " Build finished"
echo "=============================="
