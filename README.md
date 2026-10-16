# Realtek RTS3903N - Local 1080p RTSP & PTZ Control for Home Assistant

[![License: MIT](https://img.shields.io/badge/License-MIT-yellow.svg)](https://opensource.org/licenses/MIT)
[![Home Assistant](https://img.shields.io/badge/Home%20Assistant-Compatible-blue.svg)](https://www.home-assistant.io/)
[![WebRTC](https://img.shields.io/badge/WebRTC-Supported-green.svg)](https://github.com/AlexxIT/WebRTC)

Transform budget **Realtek RTS3903N** IP cameras (such as **Mibao**, **Yi IoT**, and various generic Tuya/Yi outdoor & indoor PTZ cameras) into fully local, cloud-free Home Assistant cameras with **1080p H.264 RTSP live streaming** and **Pan/Tilt (PTZ) motor control**.

---

## ✨ Features

- **1080p Full HD H.264 RTSP Stream** (`rtsp://<CAMERA_IP>/ch0_0.h264`).
- **100% Local & Private**: Completely cuts off proprietary cloud services; runs entirely in your local network without internet access.
- **Hardware Pan & Tilt Control**: Native HTTP micro-daemon running directly on the camera (`http://<CAMERA_IP>:8080/`) controlling the Ruimeng MS41909 stepper motor driver via `/dev/ssp`.
- **Zero-Copy Memory Architecture**: Bypasses sensor hardware locks by tapping directly into the Realtek ISP ring buffer (`/dev/shm/fshare_frame_buf`) with practically **0% CPU usage** and <1MB RAM.
- **Ultra-Low Latency (<250ms)**: Seamlessly integrates with Home Assistant's `custom:webrtc-camera` (go2rtc) for instant, real-time response.
- **Fully Autonomous**: Automatically boots and runs from MicroSD on power-up without requiring any external computer or manual intervention.

---

## 📷 Supported Hardware

| Component | Specification |
| :--- | :--- |
| **SoC** | Realtek RTS3903N (Taroko V0.2, MIPS-I 32-bit little-endian) |
| **RAM** | 64 MB DDR |
| **Video Sensor** | SmartSens SC2235 / SC1245 / SC2232 / SC2230 |
| **Motor Driver** | Ruimeng MS41909 (dual-axis stepper motor driver) |
| **Kernel Modules** | `rts_cam.ko`, `ssp_ms41909.ko` (`/dev/ssp`) |
| **Camera Brands** | Mibao, Yi IoT, XiaoYi Outdoor/Dome, various RTS3903N OEM cameras |

---

## 📁 Repository Structure

```text
rts3903n-ha-rtsp-ptz/
├── src/
│   ├── fshare2fifo.c       # Ring buffer reader bridging Realtek ISP memory to FIFO pipe
│   └── motor.c             # MS41909 motor driver controller & lightweight HTTP PTZ daemon
├── sdcard/
│   ├── run_rtsp.sh         # Main startup script executed on camera
│   ├── fshare2fifo         # Pre-compiled MIPS binary
│   ├── motor               # Pre-compiled MIPS binary
│   ├── rRTSPServer         # Pre-compiled RTSP server binary
│   ├── lib/                # Required shared libraries
│   └── wpa_supplicant.conf.example
├── homeassistant/
│   ├── configuration.yaml  # REST commands for PTZ motor actions
│   ├── lovelace_webrtc_card.yaml
│   └── lovelace_picture_elements_card.yaml
├── docs/
│   └── reverse_engineering.md # Technical protocol & register documentation
├── Makefile
├── build.sh                # Docker-based cross-compilation script
└── LICENSE
```

---

## 🚀 Quick Start Guide

### Step 1: Prepare the MicroSD Card
1. Format a MicroSD card (FAT32, 4GB to 32GB recommended).
2. Copy the contents of the `sdcard/` folder directly to the root of the MicroSD card:
   - `fshare2fifo`
   - `motor`
   - `rRTSPServer`
   - `run_rtsp.sh`
   - `lib/` (folder with shared libraries)
3. Insert the MicroSD card into the camera and power it on.

---

### Step 2: Enable Permanent Autostart on the Camera
Connect to the camera via Telnet (`telnet <CAMERA_IP>`):

```sh
# 1. Update /backup/init_custom.sh to start RTSP & PTZ on boot
sed -i 's/#\.\/rmm &\s*$/\.\/rmm &/g' /backup/init_custom.sh
sed -i 's/#watchdog -t 2 -T 5 \/dev\/#watchdog &/watchdog -t 2 -T 5 \/dev\/watchdog &/g' /backup/init_custom.sh
sed -i 's/\.\/stream &/sh \/tmp\/sd\/run_rtsp\.sh \&/g' /backup/init_custom.sh
sed -i 's/\.\/rRTSPServer -p 554.*//g' /backup/init_custom.sh

# 2. Commit changes to flash
sync
```

*(Note: To test without rebooting, simply run `sh /tmp/sd/run_rtsp.sh &`).*

---

### Step 3: Configure Home Assistant

#### 1. Add REST Commands to `configuration.yaml`
Add the following to your Home Assistant `configuration.yaml` (replace `CAMERA_IP` with your camera's IP address):

```yaml
rest_command:
  camera_ptz_up:
    url: "http://CAMERA_IP:8080/up?steps=50"
    method: GET

  camera_ptz_down:
    url: "http://CAMERA_IP:8080/down?steps=50"
    method: GET

  camera_ptz_left:
    url: "http://CAMERA_IP:8080/left?steps=120"
    method: GET

  camera_ptz_right:
    url: "http://CAMERA_IP:8080/right?steps=120"
    method: GET

  camera_ptz_stop:
    url: "http://CAMERA_IP:8080/stop"
    method: GET
```

Restart Home Assistant or reload REST commands under **Developer Tools ➔ YAML ➔ REST entities and notify services**.

---

#### 2. Add Lovelace Dashboard Card (WebRTC Recommended)

Using the popular [AlexxIT/WebRTC](https://github.com/AlexxIT/WebRTC) custom card:

```yaml
type: custom:webrtc-camera
url: rtsp://CAMERA_IP/ch0_0.h264
media: video
mode: webrtc,mse
ui: true
shortcuts:
  - name: Pan Left
    icon: mdi:arrow-left-bold
    service: rest_command.camera_ptz_left

  - name: Tilt Up
    icon: mdi:arrow-up-bold
    service: rest_command.camera_ptz_up

  - name: Tilt Down
    icon: mdi:arrow-down-bold
    service: rest_command.camera_ptz_down

  - name: Pan Right
    icon: mdi:arrow-right-bold
    service: rest_command.camera_ptz_right
```

---

## 🛠️ Building from Source

To compile the binaries from source for MIPS-I Linux (uClibc):

```bash
git clone https://github.com/YOUR_USERNAME/rts3903n-ha-rtsp-ptz.git
cd rts3903n-ha-rtsp-ptz

# Build using Docker with the Realtek toolchain:
./build.sh
```

---

## 🔬 Reverse Engineering Details

For a detailed technical explanation of the `/dev/shm/fshare_frame_buf` ring buffer format, slot tracking, and `/dev/ssp` stepper motor ioctl structures, refer to [`docs/reverse_engineering.md`](docs/reverse_engineering.md).

---

## 📄 License

This project is licensed under the [MIT License](LICENSE).
