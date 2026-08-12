#!/bin/bash

set -e

export BUILD_CONFIG=build.config.gki.aarch64

export KERNEL_LOCALVERSION=-oppo6.6

echo "=============================="
echo " OnePlus13 OPPO6.6 Build"
echo " ReSukiSU + SUSFS"
echo " KPM OFF"
echo "=============================="


chmod +x kernel_build/*.sh


echo "[+] Setup ReSukiSU"

kernel_build/setup_resukisu.sh


echo "[+] Setup SUSFS"

kernel_build/setup_susfs.sh


echo "[+] Apply config"

cat configs/resukisu.fragment \
>> arch/arm64/configs/xkse4_dodge_defconfig


echo "[+] Build Kernel"


build/build.sh


echo "[+] Build finished"
