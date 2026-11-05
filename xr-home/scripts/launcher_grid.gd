extends Node3D
## 3D launcher grid: one card per app from the registry. Cards are quads whose
## albedo comes from a small SubViewport-rendered card face. Hover highlights,
## click (pinch / gaze-trigger / mouse) activates.

signal app_launched(app: Dictionary)

const CARD_W: float = 0.46
const CARD_H: float = 0.29
const COLS: int = 2
const GAP: float = 0.62
const ROW_GAP: float = 0.42
const VP_SIZE := Vector2i(320, 200)

var _cards: Array[Dictionary] = []
var _hovered: int = -1


func _ready() -> void:
	var apps: Array = NuosAppRegistry.load_apps()
	for i in apps.size():
		_spawn_card(apps[i], i)


func _spawn_card(app: Dictionary, index: int) -> void:
	var col := index % COLS
	var row := index / COLS
	var pos := Vector3((col - (COLS - 1) * 0.5) * GAP, (1 - row) * ROW_GAP - 0.1, 0.0)

	var vp := SubViewport.new()
	vp.size = VP_SIZE
	vp.render_target_update_mode = SubViewport.UPDATE_ONCE
	vp.transparent_bg = true
	add_child(vp)

	var face := Control.new()
	face.set_anchors_preset(Control.PRESET_FULL_RECT)
	vp.add_child(face)

	var bg := ColorRect.new()
	bg.color = Color(0.08, 0.11, 0.15, 0.95)
	bg.set_anchors_preset(Control.PRESET_FULL_RECT)
	face.add_child(bg)

	var label := Label.new()
	label.text = str(app.get("title", "?"))
	label.position = Vector2(16, 60)
	label.add_theme_font_size_override("font_size", 30)
	face.add_child(label)

	var sub := Label.new()
	sub.text = str(app.get("subtitle", ""))
	sub.position = Vector2(16, 104)
	sub.modulate = Color(0.65, 0.72, 0.8)
	face.add_child(sub)
	_force_layout(face)
	vp.render_target_update_mode = SubViewport.UPDATE_ONCE

	var node := Node3D.new()
	node.position = pos
	add_child(node)

	# Highlight quad slightly behind the card, visible on hover.
	var glow_mat := StandardMaterial3D.new()
	glow_mat.shading_mode = BaseMaterial3D.SHADING_MODE_UNSHADED
	glow_mat.albedo_color = Color(0.18, 0.7, 1.0, 0.35)
	var glow := MeshInstance3D.new()
	var glow_mesh := QuadMesh.new()
	glow_mesh.size = Vector2(CARD_W + 0.04, CARD_H + 0.04)
	glow.mesh = glow_mesh
	glow.material_override = glow_mat
	glow.visible = false
	node.add_child(glow)

	var mesh_instance := MeshInstance3D.new()
	var quad := QuadMesh.new()
	quad.size = Vector2(CARD_W, CARD_H)
	mesh_instance.mesh = quad
	var mat := StandardMaterial3D.new()
	mat.shading_mode = BaseMaterial3D.SHADING_MODE_UNSHADED
	mat.albedo_texture = vp.get_texture()
	mesh_instance.material_override = mat
	node.add_child(mesh_instance)

	_cards.append({
		"app": app,
		"node": node,
		"glow": glow,
		"center": pos,
	})


func _force_layout(ctrl: Control) -> void:
	# SubViewports have no window size until drawn; force a size so anchors work.
	ctrl.size = Vector2(VP_SIZE)
	ctrl.position = Vector2.ZERO


func _process(_delta: float) -> void:
	# Billboard every card toward the (orbiting or head-locked) camera.
	var cam := get_viewport().get_camera_3d()
	if cam == null:
		return
	for card in _cards:
		var node: Node3D = card["node"]
		node.look_at(cam.global_position, Vector3.UP)


func update_pointer(origin: Vector3, dir: Vector3) -> void:
	# Intersect the pointer ray with the grid plane (z = 0, normal +Z).
	if absf(dir.z) < 0.0001:
		return
	var t: float = -origin.z / dir.z
	if t <= 0.0:
		_set_hover(-1)
		return
	var hit: Vector3 = origin + dir * t
	var best := -1
	for i in _cards.size():
		var c: Vector3 = _cards[i]["center"]
		if absf(hit.x - c.x) <= CARD_W * 0.5 and absf(hit.y - c.y) <= CARD_H * 0.5:
			best = i
			break
	_set_hover(best)


func activate_hovered() -> void:
	if _hovered >= 0 and _hovered < _cards.size():
		app_launched.emit(_cards[_hovered]["app"])


func _set_hover(index: int) -> void:
	if index == _hovered:
		return
	_hovered = index
	for i in _cards.size():
		var glow: Node3D = _cards[i]["glow"]
		glow.visible = (i == _hovered)
	if _hovered >= 0:
		var node: Node3D = _cards[_hovered]["node"]
		node.scale = Vector3.ONE * 1.08
	for i in _cards.size():
		if i != _hovered:
			(_cards[i]["node"] as Node3D).scale = Vector3.ONE
