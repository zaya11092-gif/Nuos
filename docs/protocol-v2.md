# NuOS v2 protocol — PhoneVR link (wire reference)

The board (`nuospvr-server`) and the phone (`PVRLink`) speak the PhoneVR
phone↔PC protocol, extended with NuOS control TLVs. All integers LE unless
noted. Implemented in `server/src/pvr_server.cpp` and
`unity/Assets/_Project/Scripts/PVR/PVRLink.cs`.

## Discovery + pairing

1. Phone UDP-announces to board:33333 every ~500 ms:
   `{'p','v','r', 0x00, uint32 version}` (version = `2 << 24`).
2. Board receives, connects **TCP to phone:33333** (phone is the TCP server —
   PhoneVR convention, NAT-friendly).
3. Board sends `PAIR_ACCEPT` (msg 3), then `HEADER_NALS` (msg 5) containing the
   Annex-B VPS/SPS/PPS NALs of the HEVC stream.
   Framing: `"pvr"` + uint8 msgType + uint16 payloadLen, then payload.
   Msg types: 0 PAIR_HMD, 1 PAIR_PHONE_CTRL, 2 PAIR_HMD_CTRLS, 3 PAIR_ACCEPT,
   4 ADDITIONAL_DATA, 5 HEADER_NALS, 6 DISCONNECT.

## Video (board → phone)

Board listens on TCP 15243; phone connects. Per frame the board writes a
64-byte header followed by one HEVC Annex-B access unit:

| off | type     | meaning                              |
|-----|----------|--------------------------------------|
| 0   | i64      | pts (µs, board monotonic)            |
| 8   | f32[4]   | head quaternion (w,x,y,z) at capture |
| 24  | uint32   | AU byte size                         |
| 28  | f32[5]   | fps telemetry (zeros in v2)          |
| 48  | f32[2]   | render/encode delay ms (zeros)       |
| 56  | i64      | wall-clock ms                        |
| 64  | bytes    | AU (`nal_size` bytes)                |

First AU after pairing is forced to an IDR on window switch.

## Pose (phone → board)

UDP 51423, 36 bytes: `f32 quat[4] (w,x,y,z) | f32 accel[3] | i64 ns` @ 60 Hz.

## NuOS v2 control TLVs (phone → board, on the control TCP)

Byte-framed: `uint8 type | uint32 len (LE) | payload`.

| type | name               | payload        | response                     |
|------|--------------------|----------------|------------------------------|
| 0x10 | LIST_WINDOWS_REQ   | —              | ADDITIONAL_DATA: JSON `[{id,title,w,h}]` |
| 0x11 | LAUNCH             | command string | ADDITIONAL_DATA: `{"launched":true}`     |
| 0x12 | STREAM_WINDOW      | uint32 window id | (starts IDR + stream)      |
