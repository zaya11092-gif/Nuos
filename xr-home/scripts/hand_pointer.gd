extends Node
## Pointer input for the launcher.
##
## XR mode: ray from the right-hand index fingertip (Vision joint order,
## wrist=0, thumb 1..4, index 5..8 -> tip = 8). Pinch = normalized
## distance(wrist, index_tip) < 0.06 to engage, hysteresis release at 0.10
## (see PROTOCOL.md "Hands").
##
## Flat mode fallback: gaze (camera center ray) + left mouse button as trigger.
##
## Hand coordinates from the bridge are normalized camera-image coords
## (x, y in [0,1], y down, top-left origin); we map them 1:1 onto screen UV of
## the home viewport. This is a v1 approximation — real alignment needs the
## camera-to-head extrinsics once those land in the pose shm.

const PINCH_ON: float = 0.06
const PINCH_OFF: float = 0.10
const JOINT_WRIST: int = 0
const JOINT_INDEX_TIP: int = 8

var camera: Camera3D
var client: Node

var pinch_active: bool = false


func _process(_delta: float) -> void:
	if client == null or camera == null:
		return
	var hands: Dictionary = client.call("get_hands")
	var present: int = int(hands.get("present", 0))
	if (present & 2) != 0:  # bit1 = right hand
		var right: PackedVector3Array = hands.get("right", PackedVector3Array())
		if right.size() > JOINT_INDEX_TIP:
			var wrist: Vector3 = right[JOINT_WRIST]
			var tip: Vector3 = right[JOINT_INDEX_TIP]
			var d: float = Vector2(wrist.x, wrist.y).distance_to(Vector2(tip.x, tip.y))
			if not pinch_active and d < PINCH_ON:
				pinch_active = true
			elif pinch_active and d > PINCH_OFF:
				pinch_active = false


## Returns {"origin": Vector3, "dir": Vector3, "valid": bool}
func current_ray() -> Dictionary:
	if camera == null:
		return {"origin": Vector3.ZERO, "dir": Vector3.FORWARD, "valid": false}
	var hands: Dictionary = client.call("get_hands") if client != null else {}
	var present: int = int(hands.get("present", 0))
	if (present & 2) != 0:
		var right: PackedVector3Array = hands.get("right", PackedVector3Array())
		if right.size() > JOINT_INDEX_TIP:
			var tip: Vector3 = right[JOINT_INDEX_TIP]
			var vp_size: Vector2 = camera.get_viewport().get_visible_rect().size
			var px := Vector2(tip.x, tip.y) * vp_size  # y already down, matches screen space
			return {
				"origin": camera.project_ray_origin(px),
				"dir": camera.project_ray_normal(px),
				"valid": true,
			}
	# Flat fallback: gaze ray.
	return {
		"origin": camera.global_position,
		"dir": -camera.global_transform.basis.z,
		"valid": true,
	}


func is_triggered() -> bool:
	if pinch_active:
		return true
	# Flat fallback: left mouse button.
	return Input.is_mouse_button_pressed(MOUSE_BUTTON_LEFT)
