extends Node
## NuOS UDP/TCP link — standalone fallback when the NuosBridge GDExtension
## is not installed (standard-Ubuntu deployment).
##
##  - Pose in:  UDP 127.0.0.1:48100, nuos_pose_t (proto/nuos_proto.h, 64 B LE)
##  - Frames out: TCP 127.0.0.1:48200 to nuos-frameudp
##      [u32 w BE][u32 h BE][u32 len BE][RGBA8 pixels]
##
## Exposes get_head_pose() with the same dictionary shape as the bridge, so
## main.gd can treat both interchangeably.

const STATE_PORT: int = 48100
const FRAME_HOST: String = "127.0.0.1"
const FRAME_PORT: int = 48200

var _udp: PacketPeerUDP
var _tcp: StreamPeerTCP
var _connected: bool = false
var _warned_frame: bool = false

var _pose: Dictionary = {
	"pos": Vector3.ZERO, "quat": Quaternion(), "t_ns": 0,
	"tracking": 0, "kiyo_ok": false, "arkit_ok": false,
}


func _ready() -> void:
	_udp = PacketPeerUDP.new()
	if _udp.bind(STATE_PORT, "0.0.0.0") == OK:
		set_process(true)
	else:
		push_warning("[udp_link] cannot bind state port 48100")


func get_head_pose() -> Dictionary:
	return _pose


func get_hands() -> Dictionary:
	return {"present": 0, "left": PackedVector3Array(), "right": PackedVector3Array(), "t_ns": 0}


func get_stats() -> Dictionary:
	return {"fps": 0.0, "bitrate_kbps": 0, "loss_ppm": 0,
			"rtt_ms": 0.0, "battery_pct": 255, "temp_c": -1.0}


func get_bridge() -> Node:
	return null  # explicit: this link IS the fallback


func _process(_delta: float) -> void:
	while _udp.get_available_packet_count() > 0:
		var pkt := _udp.get_packet()
		if pkt.size() < 64:
			continue
		var buf := StreamPeerBuffer.new()
		buf.data_array = pkt
		buf.big_endian = false
		buf.seek(8)  # skip t_mono_ns
		var pos := Vector3(buf.get_float(), buf.get_float(), buf.get_float())
		var q := Quaternion(buf.get_float(), buf.get_float(),
				buf.get_float(), buf.get_float())
		buf.seek(60)  # vel/angvel not consumed by the home in v1
		_pose = {
			"pos": pos, "quat": q, "t_ns": Time.get_ticks_usec() * 1000,
			"tracking": buf.get_u8(), "kiyo_ok": false, "arkit_ok": true,
		}


func send_frame(img: Image) -> bool:
	if img == null:
		return false
	if not _connected:
		_tcp = StreamPeerTCP.new()
		_tcp.connect_to_host(FRAME_HOST, FRAME_PORT)
		_tcp.poll()
		_connected = _tcp.get_status() == StreamPeerTCP.STATUS_CONNECTED
		if not _connected:
			if not _warned_frame:
				_warned_frame = true
				push_warning("[udp_link] frame bridge (frameudp :48200) not reachable")
			return false
		_warned_frame = false
	var data := img.get_data()
	var n := data.size()
	_tcp.put_u32(img.get_width())
	_tcp.put_u32(img.get_height())
	_tcp.put_u32(n)
	_tcp.put_data(data)
	_tcp.poll()
	if _tcp.get_status() != StreamPeerTCP.STATUS_CONNECTED:
		_connected = false
	return _connected
