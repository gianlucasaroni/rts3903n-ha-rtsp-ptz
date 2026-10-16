# RTS3903N Reverse Engineering Technical Reference

This document describes the technical internals of the Realtek RTS3903N IP camera platform, the ring buffer structure, and the hardware stepper motor driver interface.

---

## 1. System Architecture

* **SoC**: Realtek RTS3903N (Taroko V0.2, MIPS-I 32-bit little-endian, uClibc).
* **RAM**: 64 MB DDR.
* **Storage**: SPI Flash with SquashFS (`/home`, read-only) and JFFS2/YAFFS (`/backup`, read-write).
* **Video Pipeline**: Sensor (e.g. SC2235 / SC1245) -> H.264 Hardware Encoder (`rts_camera_hx280enc.ko`) -> Realtek Media Manager (`/home/app/rmm`).
* **Motor Driver IC**: Ruimeng MS41909 2-channel stepper motor controller driven by kernel module `/home/app/localko/ssp_ms41909.ko` exposing `/dev/ssp`.

---

## 2. Why Traditional Hacks Fail

Previous attempts to run standalone grabbers (such as `stream` from yi-hack) failed because:
1. `rts_cam` hardware blocks concurrent open calls once `rmm` has acquired the camera sensor.
2. Killing `rmm` triggers `/home/app/wp_cmd` supervisory reboot and hardware watchdog resets.
3. Killing the hardware watchdog (`/dev/watchdog`) directly triggers an immediate SoC hard reset.

**The Solution:** Leave `rmm` and `watchdog` alive. `rmm` encodes 1080p H.264 at 20 fps and publishes frames into a Linux shared memory ring buffer at `/dev/shm/fshare_frame_buf`. Our bridge tool (`fshare2fifo`) taps directly into this memory buffer at zero CPU cost.

---

## 3. Shared Memory Ring Buffer (`/dev/shm/fshare_frame_buf`)

* **Total Buffer Size**: 1,786,156 bytes (~1.7 MB).
* **Header Area**: Bytes 0 to 299.
  * Offset `0x04`: `valid_bytes` (32-bit uint) - total valid data in ring.
  * Offset `0x0C`: `head_offset` (32-bit uint) - writer offset.
  * Offset `0x10`: `tail_offset` (32-bit uint) - oldest record offset.
  * Offset `0x18`: `newest_seq` (32-bit uint) - current global sequence number.
  * Offset `0x1C + 16 * k`: Reader slot definitions for slots `k = 0..14`.
    * Slot state: active (1), cursor sequence, stream filter (`0x0400` for 1080p video).
* **Data Ring Area**: Begins at byte 300 (`0x12C`), size `0x1B4000` (1,785,856 bytes).

### Record Structure (`rec_hdr`, 22 bytes packed)
```c
typedef struct __attribute__((packed)) {
    uint32_t len;    /* Payload length in bytes */
    uint32_t seq;    /* Frame sequence number */
    uint32_t magic;  /* Magic signature (0x6A00xxxx) */
    uint32_t ts;     /* Hardware timestamp */
    uint16_t type;   /* 0x0400 = 1080p H.264 NAL stream */
    uint16_t chain;  /* Multi-packet chunk chain */
    uint16_t extra;  /* Flags */
} rec_hdr;
```

When `type == 0x0400`, the payload immediately contains standard Annex-B H.264 Network Abstraction Layer (NAL) units beginning with `00 00 00 01`.

---

## 4. Stepper Motor Control (`/dev/ssp`)

The kernel module `ssp_ms41909.ko` registers `/dev/ssp` as a character device:
* **Motor X (Pan)**: 4 GPIO lines (`MOTO_X_GPIO0..3`).
* **Motor Y (Tilt)**: 4 GPIO lines (`MOTO_Y_GPIO0..3`).

### `ioctl` Commands
* **Command 1**: Query current step position (returns 32-byte status array).
* **Command 2**: Execute stepper movement.
  Takes a 32-byte struct `motor_arg`:
  ```c
  struct motor_arg {
      int field0;   /* 0 */
      int field1;   /* 0 */
      int dir;      /* 1 = Tilt Up, 2 = Tilt Down, 3 = Pan Left, 4 = Pan Right */
      int speed;    /* 10 (High), 4 (Medium), 2 (Low) */
      int steps;    /* Formula: target_steps * 8 (at speed 10) */
      int field5;   /* 0 */
      int field6;   /* 0 */
      int field7;   /* 0 */
  };
  ```
* **Command 3**: Immediate stop (de-energizes all stepper motor coils).
