extends Node3D
## NuOS XR home launcher — main controller.
##
## Boot order:
##  1. Start NuosClient (bridge wrapper; safe when the GDExtension is absent).
##  2. Try OpenXR. If the interface initializes, XR mode (head pose from the
##     bridge / OpenXR, SBS stereo render for the streaming path). Otherwise
##     fall back to flat desktop mode with a slowly orbiting camera.
##  3. Build the home scene inside a SubViewport (per-eye 1280x720) and push
##     captured frames to the bridge at <= 60 fps (BRIDGE_API.md loop).
##
## Stereo note (v1): a single SubViewport is re-rendered twice per push
## (UPDATE_ONCE + frame_post_draw await) with a per-eye frustum offset, and
## both captures are blitted into one 2560x720 RGBA8 image before push_frame().
## This satisfies the bridge's max ring size (2560x800).

const EYE_WIDTH: int = 1280
const EYE_HEIGHT: int = 720
const PUSH_CAP_HZ: float = 60.0
const IPD_METERS: float = 0.064
const CAM_FOV_DEG: float = 90.0

var xr_active: bool = false
var nuos_client: Node
var udp_link: Node
var home_viewport: SubViewport
var home_root: Node3D
var camera: Camera3D
var grid: Node3D
var pointer: Node

var _flat_time: float = 0.0
var _push_accum: float = 0.0
var _push_busy: bool = false
var _push_warned: bool = false


func _ready() -> void:
	nuos_client = Node.new()
	nuos_client.name = "NuosClient"
	nuos_client.set_script(load("res://scripts/nuos_client.gd"))
	add_child(nuos_client)
	udp_link = Node.new()
	udp_link.name = "UdpLink"
	udp_link.set_script(load("res://scripts/udp_link.gd"))
	add_child(udp_link)
	_init_xr()
	_build_world()


func _init_xr() -> void:
	var xr_interface: XRInterface = XRServer.find_interface("OpenXR")
	if xr_interface == null:
		print("[xr-home] OpenXR interface not found -> flat mode")
		return
	if xr_interface.initialize():
		xr_active = true
		get_viewport().use_xr = true
		DisplayServer.window_set_vsync_mode(DisplayServer.VSYNC_DISABLED)
		print("[xr-home] OpenXR initialized")
	else:
		print("[xr-home] OpenXR init failed -> flat mode")


func _build_world() -> void:
	home_viewport = SubViewport.new()
	home_viewport.name = "HomeViewport"
	home_viewport.size = Vector2i(EYE_WIDTH, EYE_HEIGHT)
	home_viewport.own_world_3d = true
	home_viewport.render_target_update_mode = SubViewport.UPDATE_ALWAYS
	add_child(home_viewport)

	home_root = Node3D.new()
	home_root.name = "HomeRoot"
	home_viewport.add_child(home_root)

	var backdrop := Node3D.new()
	backdrop.name = "Backdrop"
	backdrop.set_script(load("res://scripts/passthrough_backdrop.gd"))
	home_root.add_child(backdrop)

	grid = Node3D.new()
	grid.name = "LauncherGrid"
	grid.set_script(load("res://scripts/launcher_grid.gd"))
	grid.connect("app_launched", _on_app_launched)
	home_root.add_child(grid)

	camera = Camera3D.new()
	camera.name = "HomeCamera"
	camera.fov = CAM_FOV_DEG
	camera.near = 0.05
	camera.far = 100.0
	camera.current = true
	home_root.add_child(camera)

	pointer = Node.new()
	pointer.name = "HandPointer"
	pointer.set_script(load("res://scripts/hand_pointer.gd"))
	pointer.set("camera", camera)
	pointer.set("client", nuos_client)
	home_root.add_child(pointer)

	if not xr_active:
		# Flat dev: mirror the SubViewport onto the window.
		var layer := CanvasLayer.new()
		add_child(layer)
		var container := SubViewportContainer.new()
		container.stretch = true
		container.set_anchors_preset(Control.PRESET_FULL_RECT)
		layer.add_child(container)
		home_viewport.reparent(container)


func _process(delta: float) -> void:
	_update_camera(delta)
	_update_pointer()
	_push_accum += delta
	if _push_accum >= 1.0 / PUSH_CAP_HZ and not _push_busy:
		_push_accum = fmod(_push_accum, 1.0 / PUSH_CAP_HZ)
		_push_busy = true
		_push_latest_frame()


func _update_camera(delta: float) -> void:
	if true:
		# Real head pose comes from the tracker's localhost state stream
		# (ARKit+Kiyo fusion) whether or not OpenXR is active.
		var pose: Dictionary = udp_link.call("get_head_pose")
		var tracking: int = int(pose.get("tracking", 0))
		if tracking > 0:
			camera.quaternion = pose.get("quat", Quaternion())
		camera.position = pose.get("pos", Vector3.ZERO)
	else:
		_flat_time += delta
		var a: float = _flat_time * 0.22
		camera.position = Vector3(sin(a) * 2.3, 0.85 + 0.12 * sin(_flat_time * 0.4), cos(a) * 2.3)
		camera.look_at(Vector3.ZERO, Vector3.UP)


func _update_pointer() -> void:
	if pointer == null or grid == null:
		return
	var ray: Dictionary = pointer.call("current_ray")
	if bool(ray.get("valid", false)):
		grid.call("update_pointer", ray["origin"], ray["dir"])
		if pointer.call("is_triggered"):
			grid.call("activate_hovered")


func _frustum_offset_px(eye_sign: int) -> Vector2:
	# Approximate stereo frustum offset: shift the frustum by half-IPD at the
	# convergence plane (~1 m) expressed in pixels. Good enough for v1 UI.
	var half_width_tan: float = tan(deg_to_rad(CAM_FOV_DEG) * 0.5) * float(EYE_WIDTH) / float(EYE_HEIGHT)
	var focal_px: float = float(EYE_WIDTH) * 0.5 / half_width_tan
	var px: float = (IPD_METERS * 0.5) * focal_px / 1.0
	return Vector2(eye_sign * px, 0.0)


func _capture_frame() -> Image:
	if not xr_active:
		# Flat mode still streams proper SBS: duplicate the eye image.
		var eye: Image = home_viewport.get_texture().get_image()
		if eye == null or eye.get_width() != EYE_WIDTH or eye.get_height() != EYE_HEIGHT:
			return eye
		if eye.get_format() != Image.FORMAT_RGBA8:
			eye.convert(Image.FORMAT_RGBA8)
		var combined := Image.create(EYE_WIDTH * 2, EYE_HEIGHT, false, Image.FORMAT_RGBA8)
		combined.blit_rect(eye, Rect2i(0, 0, EYE_WIDTH, EYE_HEIGHT), Vector2i(0, 0))
		combined.blit_rect(eye, Rect2i(0, 0, EYE_WIDTH, EYE_HEIGHT), Vector2i(EYE_WIDTH, 0))
		return combined
	# SBS: two UPDATE_ONCE passes into the same SubViewport.
	var combined: Image = Image.create(EYE_WIDTH * 2, EYE_HEIGHT, false, Image.FORMAT_RGBA8)
	for eye in 2:
		camera.frustum_offset = _frustum_offset_px(1 if eye == 0 else -1)
		home_viewport.render_target_update_mode = SubViewport.UPDATE_ONCE
		await RenderingServer.frame_post_draw
		var eye_img: Image = home_viewport.get_texture().get_image()
		if eye_img != null and eye_img.get_width() == EYE_WIDTH and eye_img.get_height() == EYE_HEIGHT:
			combined.blit_rect(eye_img, Rect2i(0, 0, EYE_WIDTH, EYE_HEIGHT), Vector2i(eye * EYE_WIDTH, 0))
	camera.frustum_offset = Vector2.ZERO
	return combined


func _push_latest_frame() -> void:
	await RenderingServer.frame_post_draw
	var img: Image = await _capture_frame()
	_push_busy = false
	if img == null:
		print("[xr-home] capture returned null")
		return
	if img.get_width() > 2560 or img.get_height() > 800:
		print("[xr-home] capture oversized: %dx%d" % [img.get_width(), img.get_height()])
		return
	if not _push_warned:
		_push_warned = true
		print("[xr-home] push path alive, img=%dx%d bridge=%s" % [img.get_width(), img.get_height(), str(nuos_client.call("get_bridge") != null)])
	var bridge: Node = nuos_client.call("get_bridge")
	if bridge != null:
		bridge.call("push_frame", img)
	else:
		udp_link.call("send_frame", img)


func _on_app_launched(app: Dictionary) -> void:
	var builtin: String = str(app.get("builtin", "stub"))
	var content: Control
	if builtin == "dashboard":
		content = Control.new()
		content.set_script(load("res://scripts/dashboard.gd"))
		content.set("client", nuos_client)
	else:
		content = _make_stub_panel(app)
	var panel := Node3D.new()
	panel.set_script(load("res://scripts/window_panel.gd"))
	home_root.add_child(panel)
	panel.call("setup", str(app.get("title", "App")), content, camera)


func _make_stub_panel(app: Dictionary) -> Control:
	var box := VBoxContainer.new()
	var title := Label.new()
	title.text = str(app.get("title", "App")) + " — not implemented yet"
	var sub := Label.new()
	sub.text = "Builtin stub (id: " + str(app.get("id", "?")) + ")"
	box.add_child(title)
	box.add_child(sub)
	return box
