extends Control
## Dashboard panel content: stream stats + tracker state from nuos_client.

var client: Node

var _rows: Dictionary = {}
var _track_label: Label
var _flags_label: Label


func _ready() -> void:
	var box := VBoxContainer.new()
	box.set_anchors_preset(Control.PRESET_FULL_RECT)
	add_child(box)

	_rows = {}
	for key: String in ["fps", "bitrate_kbps", "loss_ppm", "rtt_ms", "battery_pct", "temp_c"]:
		var lbl := Label.new()
		lbl.text = key + ": —"
		lbl.add_theme_font_size_override("font_size", 20)
		box.add_child(lbl)
		_rows[key] = lbl

	_track_label = Label.new()
	_track_label.text = "tracking: —"
	_track_label.add_theme_font_size_override("font_size", 20)
	box.add_child(_track_label)

	_flags_label = Label.new()
	_flags_label.text = "kiyo: —  arkit: —"
	_flags_label.add_theme_font_size_override("font_size", 20)
	box.add_child(_flags_label)

	var timer := Timer.new()
	timer.wait_time = 0.25
	timer.timeout.connect(_refresh)
	add_child(timer)
	timer.start()
	_refresh()


func _refresh() -> void:
	if client == null or not is_instance_valid(client):
		return
	var s: Dictionary = client.call("get_stats")
	for key: String in _rows.keys():
		var value: Variant = s.get(key, null)
		if value is float:
			_rows[key].text = "%s: %.1f" % [key, value]
		else:
			_rows[key].text = "%s: %s" % [key, str(value)]

	var pose: Dictionary = client.call("get_head_pose")
	var tracking: int = int(pose.get("tracking", 0))
	var track_str: String = ["none", "limited", "normal"][clampi(tracking, 0, 2)]
	_track_label.text = "tracking: " + track_str

	var kiyo: bool = bool(pose.get("kiyo_ok", false))
	var arkit: bool = bool(pose.get("arkit_ok", false))
	_flags_label.text = "kiyo: %s  arkit: %s" % [
		"OK" if kiyo else "DOWN",
		"OK" if arkit else "DOWN",
	]
