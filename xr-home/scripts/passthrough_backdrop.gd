extends Node3D
## v1 backdrop: a subtle dark gradient world environment.
##
## IMPORTANT: real passthrough is composited on the PHONE (the camera feed is
## overlaid on the decoded stream by the phone renderer, see PROTOCOL.md — the
## board never receives camera pixels). This node therefore provides only the
## flat/desktop-mode backdrop and a sane in-VR fallback environment; it is NOT
## the passthrough implementation.

func _ready() -> void:
	var world_env := WorldEnvironment.new()
	world_env.name = "BackdropEnv"

	var env := Environment.new()
	env.background_mode = Environment.BG_COLOR
	env.background_color = Color(0.035, 0.045, 0.06)
	env.ambient_light_source = Environment.AMBIENT_SOURCE_COLOR
	env.ambient_light_color = Color(0.4, 0.5, 0.6)
	env.ambient_light_energy = 0.5
	env.fog_enabled = true
	env.fog_light_color = Color(0.02, 0.05, 0.08)
	env.fog_density = 0.02
	world_env.environment = env
	add_child(world_env)

	# Gradient floor glow so flat mode has some spatial reference.
	var floor_mesh := MeshInstance3D.new()
	var plane := PlaneMesh.new()
	plane.size = Vector2(12.0, 12.0)
	floor_mesh.mesh = plane
	var mat := StandardMaterial3D.new()
	mat.shading_mode = BaseMaterial3D.SHADING_MODE_UNSHADED
	var grad := Gradient.new()
	grad.set_color(0, Color(0.05, 0.09, 0.13))
	grad.set_color(1, Color(0.01, 0.02, 0.03))
	var grad_tex := GradientTexture2D.new()
	grad_tex.gradient = grad
	grad_tex.fill_from = Vector2(0.5, 0.0)
	grad_tex.fill_to = Vector2(0.5, 1.0)
	mat.albedo_texture = grad_tex
	floor_mesh.material_override = mat
	floor_mesh.position = Vector3(0.0, -0.75, 0.0)
	add_child(floor_mesh)
