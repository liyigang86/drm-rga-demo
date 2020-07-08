#!/bin/sh

# export CC="arm-linux-gnueabihf-gcc -Wall"
# export CFLAGS=" --sysroot=/nvme/buildroot/output/rockchip_px3se/staging"
export CC="aarch64-linux-gnu-gcc -Wall -D_LARGEFILE_SOURCE -D_LARGEFILE64_SOURCE -D_FILE_OFFSET_BITS=64"
export CFLAGS=" --sysroot=/nvme/buildroot/output/rockchip_rk3399/staging"

make -B $@
