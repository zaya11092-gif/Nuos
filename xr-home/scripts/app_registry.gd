class_name NuosAppRegistry
extends RefCounted
## App registry for the launcher grid.
##
## Loads /etc/nuos/apps.json when present (JSON array, see README.md for the
## schema), otherwise falls back to the built-in defaults. Never throws.

const APPS_JSON_PATH: String = "/etc/nuos/apps.json"

const DEFAULT_APPS: Array = [
	{"id": "settings", "title": "Settings", "subtitle": "System dashboard", "builtin": "dashboard"},
	{"id": "files", "title": "Files-lite", "subtitle": "Browse the board", "builtin": "stub"},
	{"id": "test-pattern", "title": "TestPattern", "subtitle": "Stream checker", "builtin": "stub"},
	{"id": "about", "title": "About", "subtitle": "NuOS v1", "builtin": "stub"},
]


static func load_apps() -> Array:
	if FileAccess.file_exists(APPS_JSON_PATH):
		var f := FileAccess.open(APPS_JSON_PATH, FileAccess.READ)
		if f != null:
			var parsed: Variant = JSON.parse_string(f.get_as_text())
			if parsed is Array:
				var out: Array = []
				for entry: Variant in parsed:
					if entry is Dictionary and entry.has("id") and entry.has("title"):
						out.append(entry)
				if not out.is_empty():
					return out
	return DEFAULT_APPS.duplicate(true)
