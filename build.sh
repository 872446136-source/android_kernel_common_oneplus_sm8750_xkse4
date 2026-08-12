echo "[+] Check build system"

ls -la

echo "==== check build directory ===="

ls -la build || true


echo "[+] Build Kernel"

if [ -f build/build.sh ]; then

    echo "Using build/build.sh"

    ./build/build.sh

elif [ -f tools/bazel ]; then

    echo "Using bazel"

    tools/bazel build \
    --config=fast \
    //common:kernel_aarch64

else

    echo "No known build system found"

    exit 1

fi


echo "[+] Build finished"
