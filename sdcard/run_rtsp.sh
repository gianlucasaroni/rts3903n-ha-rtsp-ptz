#!/bin/sh
# ==============================================================================
# RTS3903N Local RTSP & PTZ Daemon Launcher
# ==============================================================================

# 1. Kill unused factory/cloud background daemons to save RAM and CPU
killall -9 arp_test log_server dispatch cloud p2p_tnp mp4record oss 2>/dev/null

# 2. Kill existing RTSP / PTZ / Audio instances if any
killall -9 rRTSPServer fshare2fifo motor audio_stream speaker_server 2>/dev/null
sleep 2

# Detect SD card directory
SD_DIR=$(cd "$(dirname "$0")" && pwd)
if [ ! -f "$SD_DIR/fshare2fifo" ]; then
    if [ -f "/tmp/sd/fshare2fifo" ]; then
        SD_DIR="/tmp/sd"
    elif [ -f "/var/tmp/sd/fshare2fifo" ]; then
        SD_DIR="/var/tmp/sd"
    fi
fi

export LD_LIBRARY_PATH="$SD_DIR/lib:/lib:/usr/lib"

echo "=== Running RTSP & PTZ from $SD_DIR ==="

chmod +x "$SD_DIR/fshare2fifo" "$SD_DIR/motor" "$SD_DIR/rRTSPServer" 2>/dev/null

# 3. Optional: Apply Wi-Fi config from SD card ONLY if Wi-Fi is not connected
if [ -f "$SD_DIR/wpa_supplicant.conf" ]; then
    if ! ifconfig wlan0 2>/dev/null | grep -q "inet addr"; then
        echo "Applying wpa_supplicant.conf from SD card..."
        cp -f "$SD_DIR/wpa_supplicant.conf" /tmp/wpa_supplicant.conf
        killall wpa_supplicant 2>/dev/null
        wpa_supplicant -c/tmp/wpa_supplicant.conf -g/tmp/wpa_supplicant-global -Dwext -iwlan0 -B
        udhcpc -i wlan0 -b -s /backup/script/default.script 2>/dev/null &
    else
        echo "Wi-Fi already connected, keeping active connection."
    fi
fi

# 4. Wait for Wi-Fi IP address
echo "Waiting for network IP..."
for i in 1 2 3 4 5 6 7 8 9 10 11 12 13 14 15 16 17 18 19 20 21 22 23 24 25 26 27 28 29 30; do
    ifconfig wlan0 2>/dev/null | grep -q "inet addr" && break
    sleep 1
done

# 5. Wait for Realtek rmm engine to create shared ring buffer
echo "Waiting for /dev/shm/fshare_frame_buf..."
for i in 1 2 3 4 5 6 7 8 9 10 11 12 13 14 15 16 17 18 19 20; do
    [ -f /dev/shm/fshare_frame_buf ] && break
    sleep 1
done

if [ ! -f /dev/shm/fshare_frame_buf ]; then
    echo "ERROR: /dev/shm/fshare_frame_buf not found! Ensure rmm is running."
    exit 1
fi

# 6. Reset reader slots 10-14 in SHM to prevent stale locks
dd if=/dev/zero of=/dev/shm/fshare_frame_buf bs=1 seek=188 count=80 conv=notrunc 2>/dev/null

# 7. Start RTSP Server on port 554
echo "Starting rRTSPServer on port 554..."
cd "$SD_DIR"
./rRTSPServer -p 554 -d >/tmp/rtsp.log 2>&1 &
sleep 1

# 8. Start Ring-Buffer-to-FIFO bridge (1080p H.264 stream)
echo "Starting fshare2fifo bridge..."
"$SD_DIR/fshare2fifo" -f /tmp/h264_high_fifo >/tmp/f2f.log 2>&1 &
sleep 1

# 9. Start Local PTZ HTTP Daemon on port 8080
echo "Starting PTZ HTTP Daemon on port 8080..."
"$SD_DIR/motor" -d 8080 >/tmp/ptz.log 2>&1 &
sleep 1

# 10. Re-kill any cloud daemons that might have been spawned during boot
killall -9 arp_test log_server dispatch cloud p2p_tnp mp4record oss 2>/dev/null

echo "=== All services started successfully ==="
ps | grep -E "rRTSPServer|fshare2fifo|motor"

