extends Object
## File logger that bypasses stdout buffering (nohup-safe).
static func log(msg: String) -> void:
	var f := FileAccess.open("/tmp/xrhome-dbg.log", FileAccess.WRITE_READ if not FileAccess.file_exists("/tmp/xrhome-dbg.log") else FileAccess.READ_WRITE)
	if f == null:
		return
	f.seek_end()
	f.store_line(Time.get_ticks_msec() / 1000.0 + " " + msg)
