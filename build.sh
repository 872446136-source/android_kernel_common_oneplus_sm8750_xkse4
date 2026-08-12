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

    cat configs/resukisu.fragment >> arch/arm64/configs/xkse4_dodge_defconfig

else

    echo "No resukisu.fragment found"

fi


echo "[+] Check build environment"

ls -la

echo "=============================="
echo " Search bazel tools"
echo "=============================="


find . -maxdepth 3 \
-name "bazel*" \
-type f \
| head -20



echo "[+] Build Kernel"



# Android kernel Kleaf build

if [ -f tools/bazel ]; then

    echo "Using Android tools/bazel"


    tools/bazel build \
    --config=fast \
    //:kernel_aarch64



elif [ -f bazel/bazelisk.sh ]; then


    echo "Using bazelisk"


    bazel/bazelisk.sh build \
    --config=fast \
    //:kernel_aarch64



else


    echo "No bazel wrapper"


    echo "Using GKI build.sh"


    if [ -f build/build.sh ]; then


        ./build/build.sh


    else


        echo "Fallback make build"


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


fi



echo "=============================="
echo " Build finished"
echo "=============================="
