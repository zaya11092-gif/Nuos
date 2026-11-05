extends Node
## NuosBridge wrapper. Owns the GDExtension NuosBridge node if the class was
## registered by platform/gdext-nuos-bridge; otherwise hands out safe dummy
## dictionaries so flat/desktop dev keeps working with no bridge present.
##
## Cached values are refreshed by a poll timer (bridge itself polls shm at
## 10 Hz; matching the tracker state rate from BRIDGE_API.md).

const POLL_HZ: float = 10.0

var bridge: Node = null

var head_pose: Dictionary = {
	"pos": Vector3.ZERO,
	"quat": Quaternion(),
	"t_ns": 0,
	"tracking": 0,
	"kiyo_ok": false,
	"arkit_ok": false,
}

var hands: Dictionary = {
	"present": 0,
	"left": PackedVector3Array(),
	"right": PackedVector3Array(),
	"t_ns": 0,
}

var stats: Dictionary = {
	"fps": 0.0,
	"bitrate_kbps": 0,
	"loss_ppm": 0,
	"rtt_ms": 0.0,
	"battery_pct": 0,
	"temp_c": 0.0,
}


func _ready() -> void:
	if ClassDB.class_exists("NuosBridge"):
		var inst: Variant = ClassDB.instantiate("NuosBridge")
		if inst is Node:
			bridge = inst
			add_child(bridge)  # bridge connects/creates shm in its _ready()
			print("[nuos-client] NuosBridge GDExtension loaded")
	else:
		print("[nuos-client] NuosBridge absent — serving dummy data")
	_fill_dummy_hands()

	var timer := Timer.new()
	timer.wait_time = 1.0 / POLL_HZ
	timer.timeout.connect(_poll)
	add_child(timer)
	timer.start()
	_poll()


func get_bridge() -> Node:
	return bridge


func get_head_pose() -> Dictionary:
	return head_pose


func get_hands() -> Dictionary:
	return hands


func get_stats() -> Dictionary:
	return stats


func _poll() -> void:
	if bridge == null or not is_instance_valid(bridge):
		return
	var p: Variant = bridge.call("get_head_pose")
	if p is Dictionary and not p.is_empty():
		head_pose = p
	var h: Variant = bridge.call("get_hands")
	if h is Dictionary and not h.is_empty():
		_fill_hand_arrays(h)
		hands = h
	var s: Variant = bridge.call("get_stats")
	if s is Dictionary and not s.is_empty():
		stats = s


func _fill_hand_arrays(h: Dictionary) -> void:
	# Guarantee 21-entry PackedVector3Array even if the bridge sends less.
	for side: String in ["left", "right"]:
		var arr: Variant = h.get(side, PackedVector3Array())
		if not (arr is PackedVector3Array):
			arr = PackedVector3Array()
		while (arr as PackedVector3Array).size() < 21:
			(arr as PackedVector3Array).append(Vector3.ZERO)
		h[side] = arr


func _fill_dummy_hands() -> void:
	if hands["left"] is PackedVector3Array and (hands["left"] as PackedVector3Array).size() == 21:
		return
	hands["left"] = _zero_hand()
	hands["right"] = _zero_hand()


static func _zero_hand() -> PackedVector3Array:
	var arr := PackedVector3Array()
	arr.resize(21)
	return arr
