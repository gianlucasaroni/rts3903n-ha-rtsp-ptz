#!/bin/sh
# ==============================================================================
# RTS3903N Local RTSP & PTZ Daemon Launcher
# https://github.com/
# ==============================================================================

# Kill existing instances if any
killall -9 rRTSPServer fshare2fifo motor 2>/dev/null
sleep 1

# Detect SD card directory
SD_DIR=$(cd "$(dirname "$0")" && pwd)
if [ ! -f "$SD_DIR/fshare2fifo" ]; then
    if [ -f "/tmp/sd/fshare2fifo" ]; then
        SD_DIR="/tmp/sd"
    elif [ -f "/var/tmp/sd/fshare2fifo" ]; then
        SD_DIR="/var/tmp/sd"
    fi
fi

echo "=== Running RTSP & PTZ from $SD_DIR ==="

chmod +x "$SD_DIR/fshare2fifo" "$SD_DIR/motor" "$SD_DIR/rRTSPServer" 2>/dev/null

# 1. Wait for Realtek rmm engine to create shared ring buffer
echo "Waiting for /dev/shm/fshare_frame_buf..."
for i in 1 2 3 4 5 6 7 8 9 10 11 12 13 14 15 16 17 18 19 20; do
    [ -f /dev/shm/fshare_frame_buf ] && break
    sleep 1
done

if [ ! -f /dev/shm/fshare_frame_buf ]; then
    echo "ERROR: /dev/shm/fshare_frame_buf not found! Ensure rmm is running."
    exit 1
fi

# 2. Start RTSP Server on port 554
echo "Starting rRTSPServer on port 554..."
export LD_LIBRARY_PATH="$SD_DIR/lib:/home/app/lib:/lib:/usr/lib"
cd "$SD_DIR"
./rRTSPServer -p 554 -d >/tmp/rtsp.log 2>&1 &
sleep 1

# 3. Start Ring-Buffer-to-FIFO bridge (1080p H.264 stream)
echo "Starting fshare2fifo bridge..."
"$SD_DIR/fshare2fifo" -f /tmp/h264_high_fifo >/tmp/f2f.log 2>&1 &
sleep 1

# 4. Start Local PTZ HTTP Daemon on port 8080
echo "Starting PTZ HTTP Daemon on port 8080..."
"$SD_DIR/motor" -d 8080 >/tmp/ptz.log 2>&1 &
sleep 1

echo "=== All services started successfully ==="
ps | grep -E "rRTSPServer|fshare2fifo|motor"
