extends Node3D
## Reusable floating panel: SubViewport-rendered UI on a quad, with a title
## bar and a close button. Content is any Control built by the caller.

signal closed

const PANEL_SIZE := Vector2i(768, 512)
const PANEL_W: float = 1.15
const PANEL_H: float = 0.767  # keep aspect = 768/512
const TITLE_H: int = 56

var viewport: SubViewport
var _content: Control


func setup(title: String, content: Control, cam: Camera3D) -> void:
	viewport = SubViewport.new()
	viewport.size = PANEL_SIZE
	viewport.render_target_update_mode = SubViewport.UPDATE_ALWAYS
	viewport.transparent_bg = false
	add_child(viewport)

	var root := Control.new()
	root.set_anchors_preset(Control.PRESET_FULL_RECT)
	root.size = Vector2(PANEL_SIZE)
	viewport.add_child(root)

	var bg := ColorRect.new()
	bg.color = Color(0.07, 0.09, 0.13, 0.97)
	bg.set_anchors_preset(Control.PRESET_FULL_RECT)
	root.add_child(bg)

	# Title bar
	var bar := PanelContainer.new()
	bar.position = Vector2.ZERO
	bar.size = Vector2(PANEL_SIZE.x, TITLE_H)
	var bar_style := StyleBoxFlat.new()
	bar_style.bg_color = Color(0.12, 0.16, 0.22)
	bar.add_theme_stylebox_override("panel", bar_style)
	root.add_child(bar)

	var bar_box := HBoxContainer.new()
	bar_box.set_anchors_preset(Control.PRESET_FULL_RECT)
	bar_box.add_theme_constant_override("margin_left", 14)
	bar_box.add_theme_constant_override("margin_right", 10)
	bar.add_child(bar_box)

	var label := Label.new()
	label.text = title
	label.vertical_alignment = VERTICAL_ALIGNMENT_CENTER
	label.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	label.add_theme_font_size_override("font_size", 26)
	bar_box.add_child(label)

	var close_btn := Button.new()
	close_btn.text = "X"
	close_btn.custom_minimum_size = Vector2(48, 40)
	close_btn.pressed.connect(func() -> void: closed.emit())
	bar_box.add_child(close_btn)

	# Content area
	content.position = Vector2(12, TITLE_H + 10)
	content.size = Vector2(PANEL_SIZE.x - 24, PANEL_SIZE.y - TITLE_H - 20)
	root.add_child(content)

	var mesh_instance := MeshInstance3D.new()
	var quad := QuadMesh.new()
	quad.size = Vector2(PANEL_W, PANEL_H)
	mesh_instance.mesh = quad
	var mat := StandardMaterial3D.new()
	mat.shading_mode = BaseMaterial3D.SHADING_MODE_UNSHADED
	mat.albedo_texture = viewport.get_texture()
	mesh_instance.material_override = mat
	add_child(mesh_instance)

	# Face the camera, offset to the right side so the grid stays visible.
	if cam != null:
		look_at(cam.global_position, Vector3.UP)
		global_position = cam.global_position + (-global_transform.basis.z) * 1.4 + global_transform.basis.x * 0.55
		look_at(cam.global_position, Vector3.UP)

	closed.connect(_on_closed)


var _closing: bool = false


func _on_closed() -> void:
	if _closing:
		return
	_closing = true
	queue_free()
	closed.emit()
