#!/bin/bash
set -e

# ==============================================================================
# Cross-compilation script using Docker for Realtek RTS3903N (MIPS-I uClibc)
# ==============================================================================

echo "=== Building MIPS-I binaries via Docker ==="

# Download or extract toolchain if not present
TOOLCHAIN_DIR="./rsdk-4.8.5-5281-EL-3.10-u0.9.33-m32fut-161202"

if [ ! -d "$TOOLCHAIN_DIR" ]; then
    echo "Toolchain directory $TOOLCHAIN_DIR not found in current directory."
    echo "Please place the Realtek MIPS toolchain in the project directory."
    exit 1
fi

docker run --rm --platform linux/386 \
  -v "$(pwd)":/work \
  debian:bookworm-slim \
  sh -c "export PATH=/work/$TOOLCHAIN_DIR/bin:\$PATH && \
         cd /work && \
         mips-linux-uclibc-gcc -std=gnu99 -O2 -s src/fshare2fifo.c -o sdcard/fshare2fifo && \
         mips-linux-uclibc-gcc -std=gnu99 -O2 -s src/motor.c -o sdcard/motor"

echo "=== Build complete! Binaries placed in sdcard/ ==="
ls -lh sdcard/fshare2fifo sdcard/motor
