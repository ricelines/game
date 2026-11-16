extends Control

const Stream = preload("res://protocol.gd")
const RecordFeed = preload("res://record_feed.gd")
const UIProbe = preload("res://ui_probe.gd")
const UITheme = preload("res://ui_theme.gd")
const ConnectionEntry = preload("res://connection_entry.gd")
const InspectionText = preload("res://inspection_text.gd")
const AssetCache = preload("res://assets.gd")
const WorldView = preload("res://world_view.gd")
const INK = Color(0.035, 0.065, 0.08, 0.94)
const PAPER = Color(0.93, 0.91, 0.85)
const MUTED = Color(0.55, 0.65, 0.66)
const AMBER = Color(0.95, 0.69, 0.35)
var protocol = Stream.new()
var assets
var world
var camera: Camera3D
var viewport: SubViewport
var socket = WebSocketPeer.new()
var ws_url = "ws://127.0.0.1:8080/ws"
var server_base = "http://127.0.0.1:8080"
var key = ""
var mode = "watch"
var gateway_url = ""
var purpose_token = ""
var browser_diagnostics = false
var connected = false
var hello_sent = false
var reconnect_ms = 0
var request_id = 0
var inspect_id = -1
var inspect_details: Dictionary = {}
var event_since = 0
var trace_root = 0
var trace_thing = ""
var terms: Dictionary = {}
var models: Dictionary = {}
var materials: Dictionary = {}
var objects: Dictionary = {}
var agents: Dictionary = {}
var records: Dictionary = {}
var follow_handle = ""
var selected = ""
var target = Vector3.ZERO
var orbit_yaw = 0.55
var orbit_pitch = 0.48
var distance = 28.0
var render_scale = 0.65
var exposure_lux = 8.0
var network_ms = 0.0
var network_packets = 0
var _framed = false
var _orbit = false
var _pan = false
var _dragged = false
var _click_start = Vector2.ZERO
var _last_view = Vector3.INF
var _clock = 0.0
var _events_clock = 0.0
var _gateway_clock = 0.0
var _debug = false
var _feed_dirty = false
var _status: Label
var _stats: Label
var _left: PanelContainer
var _left_content: VBoxContainer
var _inspector: PanelContainer
var _inspect_title: Label
var _inspect_body: RichTextLabel
var _feed: RichTextLabel
var _tab = "residents"
var _follow_button: Button
var _purpose_box: TextEdit
var _purpose_agent: OptionButton
var _purpose_status: Label
var _purpose_send: Button
var _toast: Label
var _empty: Label
var _entry


func _ready() -> void:
	set_anchors_and_offsets_preset(Control.PRESET_FULL_RECT)
	_parse_location()
	_build_world()
	_build_ui()
	if key.is_empty():
		_entry.open()
	else:
		_connect()


func _parse_location() -> void:
	if OS.has_feature("web"):
		var origin = str(JavaScriptBridge.eval("window.location.origin"))
		ws_url = origin.replace("https://", "wss://").replace("http://", "ws://") + "/ws"
		var query = str(JavaScriptBridge.eval("window.location.search")).trim_prefix("?")
		for part in query.split("&", false):
			var pair = part.split("=", true, 1)
			if pair.size() != 2:
				continue
			var value = pair[1].uri_decode()
			match pair[0]:
				"ws":
					ws_url = value
				"key":
					key = value
				"mode":
					mode = value
				"gateway":
					gateway_url = value
				"purpose_token":
					purpose_token = value
				"quality":
					render_scale = 0.5 if value == "low" else (1.0 if value == "high" else 0.65)
				"exposure_lux":
					exposure_lux = maxf(0.001, value.to_float())
		var supplied = JSON.parse_string(
			str(JavaScriptBridge.eval("JSON.stringify(window.ambergris_config || {})"))
		)
		if supplied is Dictionary:
			key = str(supplied.get("key", key))
			purpose_token = str(supplied.get("purpose_token", purpose_token))
			mode = str(supplied.get("mode", mode))
			gateway_url = str(supplied.get("gateway", gateway_url))
			browser_diagnostics = bool(supplied.get("diagnostics", false))
			ws_url = str(supplied.get("ws", ws_url))
			exposure_lux = maxf(0.001, float(supplied.get("exposure_lux", exposure_lux)))
	server_base = ws_url.replace("wss://", "https://").replace("ws://", "http://").trim_suffix(
		"/ws"
	)


func _build_world() -> void:
	viewport = SubViewport.new()
	viewport.own_world_3d = true
	viewport.render_target_update_mode = SubViewport.UPDATE_ALWAYS
	viewport.handle_input_locally = false
	add_child(viewport)
	var image = TextureRect.new()
	image.texture = viewport.get_texture()
	image.expand_mode = TextureRect.EXPAND_IGNORE_SIZE
	image.stretch_mode = TextureRect.STRETCH_SCALE
	image.mouse_filter = Control.MOUSE_FILTER_IGNORE
	image.set_anchors_and_offsets_preset(Control.PRESET_FULL_RECT)
	add_child(image)
	assets = AssetCache.new()
	add_child(assets)
	assets.failed.connect(func(_hash, reason): _notify("Asset pending: " + reason))
	world = WorldView.new()
	viewport.add_child(world)
	world.setup(protocol, assets)
	world.models = models
	world.materials = materials
	world.metadata = objects
	world.terms = terms
	world.exposure_lux = exposure_lux
	var environment = WorldEnvironment.new()
	var settings = Environment.new()
	settings.background_mode = Environment.BG_COLOR
	settings.background_color = Color(0.025, 0.055, 0.065)
	settings.ambient_light_source = Environment.AMBIENT_SOURCE_DISABLED
	settings.tonemap_mode = Environment.TONE_MAPPER_LINEAR
	environment.environment = settings
	world.add_child(environment)
	camera = Camera3D.new()
	camera.fov = 52
	camera.near = 0.08
	camera.far = 1000
	world.add_child(camera)
	camera.current = true
	resized.connect(_resize_view)
	_resize_view()


func _resize_view() -> void:
	if viewport:
		viewport.size = Vector2i(maxf(320, size.x * render_scale), maxf(240, size.y * render_scale))


func _build_ui() -> void:
	var brand = VBoxContainer.new()
	brand.position = Vector2(30, 25)
	brand.add_theme_constant_override("separation", 3)
	add_child(brand)
	brand.add_child(UITheme.label("AMBERGRIS", 29, PAPER))
	brand.add_child(UITheme.label("A WORLD, STILL BECOMING", 10, AMBER))
	var status_panel = PanelContainer.new()
	status_panel.position = Vector2(340, 26)
	status_panel.add_theme_stylebox_override("panel", UITheme.style(INK, 30))
	add_child(status_panel)
	_status = UITheme.label("CONNECTING TO THE WORLD", 11, MUTED)
	status_panel.add_child(_status)
	var modes = HBoxContainer.new()
	modes.set_anchors_and_offsets_preset(Control.PRESET_TOP_RIGHT)
	modes.position = Vector2(-390, 26)
	modes.add_theme_constant_override("separation", 8)
	add_child(modes)
	modes.add_child(UITheme.button("Free camera", _free_camera))
	modes.add_child(UITheme.button("Home", _frame_world))
	modes.add_child(UITheme.button("Debug", _toggle_debug))
	modes.add_child(UITheme.button("Quality", _cycle_quality))
	_left = PanelContainer.new()
	_left.set_anchors_and_offsets_preset(Control.PRESET_LEFT_WIDE)
	_left.offset_left = 24
	_left.offset_right = 322
	_left.offset_top = 116
	_left.offset_bottom = -82
	_left.add_theme_stylebox_override("panel", UITheme.style(INK))
	add_child(_left)
	var sidebar = VBoxContainer.new()
	sidebar.add_theme_constant_override("separation", 16)
	_left.add_child(sidebar)
	var tabs = HBoxContainer.new()
	sidebar.add_child(tabs)
	tabs.add_child(UITheme.button("Residents", _show_tab.bind("residents")))
	tabs.add_child(UITheme.button("Record", _show_tab.bind("record")))
	tabs.add_child(UITheme.button("Explore", _show_tab.bind("explore")))
	var scroll = ScrollContainer.new()
	scroll.size_flags_vertical = Control.SIZE_EXPAND_FILL
	scroll.horizontal_scroll_mode = ScrollContainer.SCROLL_MODE_DISABLED
	sidebar.add_child(scroll)
	_left_content = VBoxContainer.new()
	_left_content.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	_left_content.add_theme_constant_override("separation", 14)
	scroll.add_child(_left_content)
	_inspector = PanelContainer.new()
	_inspector.set_anchors_and_offsets_preset(Control.PRESET_RIGHT_WIDE)
	_inspector.offset_left = -354
	_inspector.offset_right = -24
	_inspector.offset_top = 116
	_inspector.offset_bottom = -82
	_inspector.add_theme_stylebox_override("panel", UITheme.style(INK))
	add_child(_inspector)
	var info = VBoxContainer.new()
	info.add_theme_constant_override("separation", 12)
	_inspector.add_child(info)
	var heading = HBoxContainer.new()
	info.add_child(heading)
	_inspect_title = UITheme.label("An object in the world", 18)
	_inspect_title.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	_inspect_title.text_overrun_behavior = TextServer.OVERRUN_TRIM_ELLIPSIS
	heading.add_child(_inspect_title)
	heading.add_child(UITheme.button("×", _close_inspector))
	var tools = HBoxContainer.new()
	info.add_child(tools)
	tools.add_child(UITheme.button("Focus", func(): _focus(selected)))
	_follow_button = UITheme.button("Follow", func(): _follow(selected))
	tools.add_child(_follow_button)
	tools.add_child(UITheme.button("Trace effects", func(): _request_events(selected)))
	_inspect_body = RichTextLabel.new()
	_inspect_body.bbcode_enabled = false
	_inspect_body.selection_enabled = true
	_inspect_body.size_flags_vertical = Control.SIZE_EXPAND_FILL
	_inspect_body.add_theme_color_override("default_color", PAPER)
	_inspect_body.add_theme_font_size_override("normal_font_size", 13)
	info.add_child(_inspect_body)
	_inspector.hide()
	var footer = PanelContainer.new()
	footer.set_anchors_and_offsets_preset(Control.PRESET_BOTTOM_LEFT)
	footer.position = Vector2(24, -60)
	footer.add_theme_stylebox_override("panel", UITheme.style(Color(0.03, 0.06, 0.075, 0.78), 10))
	add_child(footer)
	footer.add_child(
		UITheme.label(
			"DRAG  orbit     W A S D  pan     WHEEL  zoom     CLICK  inspect     ALT  select glass",
			11,
			MUTED
		)
	)
	_stats = UITheme.label("", 11, MUTED)
	_stats.set_anchors_and_offsets_preset(Control.PRESET_BOTTOM_RIGHT)
	_stats.position = Vector2(-390, -40)
	_stats.size = Vector2(365, 20)
	_stats.horizontal_alignment = HORIZONTAL_ALIGNMENT_RIGHT
	add_child(_stats)
	_toast = UITheme.label("", 13, AMBER)
	_toast.set_anchors_and_offsets_preset(Control.PRESET_CENTER_BOTTOM)
	_toast.position = Vector2(-270, -94)
	_toast.size = Vector2(540, 26)
	_toast.horizontal_alignment = HORIZONTAL_ALIGNMENT_CENTER
	add_child(_toast)
	_empty = UITheme.label("The world is connecting.\nIts inhabitants will appear here.", 20, PAPER)
	_empty.set_anchors_and_offsets_preset(Control.PRESET_CENTER)
	_empty.position = Vector2(-180, -30)
	_empty.size = Vector2(460, 90)
	_empty.horizontal_alignment = HORIZONTAL_ALIGNMENT_CENTER
	add_child(_empty)
	_sidebar()
	_entry = ConnectionEntry.new()
	_entry.setup(mode)
	_entry.submitted.connect(_submit_connection)
	add_child(_entry)
	_entry.hide()


func _submit_connection(supplied: String, view_mode: String) -> void:
	key = supplied
	mode = view_mode
	_connect()


func _sidebar() -> void:
	for child in _left_content.get_children():
		child.queue_free()
	_feed = null
	_purpose_box = null
	_purpose_status = null
	_purpose_send = null
	if _tab == "record":
		_left_content.add_child(UITheme.label("What leaves a trace", 20))
		var subtitle = UITheme.label("Actions, consequences, and their authors.", 12, MUTED)
		subtitle.autowrap_mode = TextServer.AUTOWRAP_WORD_SMART
		_left_content.add_child(subtitle)
		_feed = RichTextLabel.new()
		_feed.bbcode_enabled = true
		_feed.meta_clicked.connect(_trace_event)
		_feed.fit_content = true
		_feed.custom_minimum_size = Vector2(250, 200)
		_feed.add_theme_font_size_override("normal_font_size", 13)
		_left_content.add_child(_feed)
		_left_content.add_child(UITheme.button("Recent record", _recent_record))
		_update_feed()
	elif _tab == "explore":
		_left_content.add_child(UITheme.label("Read the world", 20))
		var search = LineEdit.new()
		search.placeholder_text = "Find a named object…"
		_left_content.add_child(search)
		var results = VBoxContainer.new()
		_left_content.add_child(results)
		search.text_changed.connect(_search_objects.bind(results))
		_left_content.add_child(UITheme.label("OVERLAYS", 10, AMBER))
		_left_content.add_child(
			UITheme.button("Object bounds", func(): world.show_bounds = not world.show_bounds)
		)
		_left_content.add_child(
			UITheme.button("Interest cells", func(): world.show_cells = not world.show_cells)
		)
		_left_content.add_child(UITheme.button("Navigation", _request_navigation))
	else:
		_left_content.add_child(UITheme.label("Other minds, nearby", 20))
		if agents.is_empty():
			var waiting = (
				UITheme
				. label(
					"Waiting for inhabitants. Their purposes and decisions come from the connected world.",
					13,
					MUTED
				)
			)
			waiting.autowrap_mode = TextServer.AUTOWRAP_WORD_SMART
			_left_content.add_child(waiting)
		for id in agents:
			var agent = agents[id]
			var card = PanelContainer.new()
			card.add_theme_stylebox_override(
				"panel", UITheme.style(Color(0.09, 0.14, 0.16, 0.9), 10)
			)
			_left_content.add_child(card)
			var box = VBoxContainer.new()
			box.add_theme_constant_override("separation", 7)
			card.add_child(box)
			box.add_child(UITheme.label(str(agent.get("name", id)), 17))
			box.add_child(
				UITheme.label(str(agent.get("status", "in the world")).to_upper(), 10, AMBER)
			)
			var purpose = UITheme.label(str(agent.get("purpose", "")), 12, MUTED)
			purpose.autowrap_mode = TextServer.AUTOWRAP_WORD_SMART
			purpose.custom_minimum_size.x = 210
			box.add_child(purpose)
			box.add_child(
				UITheme.button(
					"Follow " + str(agent.get("name", id)),
					func(): _follow(str(agent.get("body", "")))
				)
			)
		_left_content.add_child(UITheme.label("GIVE AN AGENT A PURPOSE", 10, AMBER))
		_purpose_agent = OptionButton.new()
		for id in agents:
			_purpose_agent.add_item(str(agents[id].get("name", id)))
			_purpose_agent.set_item_metadata(_purpose_agent.item_count - 1, id)
		_left_content.add_child(_purpose_agent)
		_purpose_box = TextEdit.new()
		_purpose_box.placeholder_text = "What would you like your agent to pursue?"
		_purpose_box.custom_minimum_size = Vector2(245, 90)
		_purpose_box.wrap_mode = TextEdit.LINE_WRAPPING_BOUNDARY
		_left_content.add_child(_purpose_box)
		_purpose_send = UITheme.button("Send purpose", _send_purpose, true)
		_left_content.add_child(_purpose_send)
		_purpose_status = UITheme.label("", 11, MUTED)
		_purpose_status.autowrap_mode = TextServer.AUTOWRAP_WORD_SMART
		_left_content.add_child(_purpose_status)


func _connect() -> void:
	if key.is_empty():
		return
	if socket.get_ready_state() != WebSocketPeer.STATE_CLOSED:
		socket.close()
	protocol.reset()
	socket = WebSocketPeer.new()
	socket.inbound_buffer_size = 8 * 1024 * 1024
	hello_sent = false
	connected = false
	var error = socket.connect_to_url(ws_url)
	if error != OK:
		_notify("Connection: " + error_string(error))
	reconnect_ms = Time.get_ticks_msec() + 2000


func _send(message: Dictionary) -> int:
	request_id += 1
	message.id = request_id
	if socket.get_ready_state() == WebSocketPeer.STATE_OPEN:
		socket.send_text(JSON.stringify(message))
	return request_id


func _text(message: Dictionary) -> void:
	match str(message.get("t", "")):
		"welcome":
			connected = true
			_entry.hide()
			protocol.configure(message)
			assets.root_url = str(message.get("assets", server_base + "/assets/"))
			if assets.root_url.begins_with("/"):
				assets.root_url = server_base + assets.root_url
			gateway_url = str(message.get("agent_gateway", gateway_url))
			_send({"t": "view", "pos": _server_array(target)})
		"terrain":
			world.add_terrain(message)
		"catalog":
			for term in message.get("terms", []):
				terms[str(int(term.id))] = term
				terms[str(term.name)] = term
			for material in message.get("materials", []):
				materials[int(material.id)] = material
			for model in message.get("models", []):
				models[int(model.id)] = model
			world.world_ambient_lux = float(message.get("world", {}).get("ambient_lux", world.world_ambient_lux))
			world.invalidate_model_data()
		"metadata":
			for row in message.get("objects", []):
				if objects.get(str(row.h), {}) != row:
					objects[str(row.h)] = row
					world.remove(str(row.h))
					world.invalidate(str(row.h))
			if message.has("agents"):
				_set_agents(message.agents)
		"events":
			for event in message.get("events", []):
				records[int(event.id)] = event
				event_since = maxi(event_since, int(event.id))
			_feed_dirty = true
			if not selected.is_empty() and message.has("thing"):
				_update_inspector(message)
		"event":
			if message.has("id") and message.has("kind"):
				records[int(message.id)] = message
				event_since = maxi(event_since, int(message.id))
				_feed_dirty = true
		"debug":
			world.debug_segments = message.get("segments", [])
			for triangle in message.get("triangles", []):
				for index in 3:
					world.debug_segments.append([triangle[index], triangle[(index + 1) % 3]])
		"inspection", "result":
			if int(message.get("id", -2)) == inspect_id:
				_update_inspector(message)
		"bye":
			var reason = str(message.get("reason", "Connection closed"))
			if reason == "bad_key" or reason == "mode_not_allowed":
				key = ""
				connected = false
				_entry.open(
					(
						"That key was not accepted. Check it and try again."
						if reason == "bad_key"
						else "This key cannot use that view. Choose Spectator or another key."
					)
				)
			else:
				_notify(reason)


func _set_agents(rows: Array) -> void:
	var changed = JSON.stringify(agents.values()) != JSON.stringify(rows)
	agents.clear()
	for row in rows:
		agents[str(row.id)] = row
	if changed and _tab == "residents":
		_sidebar()


func _name(handle: String) -> String:
	var metadata = objects.get(handle, {})
	var model = models.get(int(metadata.get("model", 0)), {})
	return str(metadata.get("text", {}).get("name", model.get("text", {}).get("name", handle)))


func _select(handle: String) -> void:
	if handle.is_empty():
		return
	selected = handle
	inspect_details = {}
	world.selected = handle
	_inspector.show()
	_inspect_title.text = _name(handle)
	_update_inspector({})
	inspect_id = _send({"t": "inspect", "target": handle})


func _update_inspector(details: Dictionary) -> void:
	if selected.is_empty() or not protocol.entities.has(selected):
		return
	if not details.is_empty():
		inspect_details = details
	var row = protocol.entities[selected]
	var metadata = objects.get(selected, {})
	var model = models.get(int(metadata.get("model", 0)), {})
	var says = metadata.get("text", model.get("text", {}))
	var text = selected + "\n\nMAKER'S WORDS\n" + str(says.get("description", ""))
	var inspection = inspect_details.get("record", inspect_details)
	text += InspectionText.elaborated(inspection)
	text += "\n\nMEASURED IN THE WORLD\n"
	var position = row.position
	text += "Position  %.2f, %.2f, %.2f m\n" % [position.x, position.y, position.z]
	var manifest = assets.cache.get(row.asset, {}).get("manifest", {})
	if manifest.has("size"):
		var dimensions = manifest.size
		text += "Size  %.2f × %.2f × %.2f m\n" % [dimensions[0], dimensions[1], dimensions[2]]
	if manifest.has("volume"):
		text += "Solid volume  %.5f m³\n" % float(manifest.volume)
	if manifest.has("capacity"):
		text += "Capacity  %.3f L\n" % (float(manifest.capacity) * 1000)
	text += "Owner  " + str(metadata.get("owner", "—")) + "\n"
	text += "\nDECLARED STATE\n"
	for term in row.states:
		var declaration = terms.get(str(term), {})
		text += (
			"%s  %s %s\n"
			% [
				str(declaration.get("name", term)),
				str(snappedf(float(row.states[term]), 0.0001)),
				str(declaration.get("unit", ""))
			]
		)
	if row.states.is_empty():
		text += "No replicated state.\n"
	if not inspect_details.is_empty():
		text += InspectionText.world_record(inspection, _debug)
	var nearby: Array = []
	for event in records.values():
		if selected in event.get("things", []):
			nearby.append(event)
	if not nearby.is_empty():
		text += "\nRECENT CAUSAL RECORD\n"
		for event in nearby.slice(maxi(0, nearby.size() - 8)):
			text += (
				"#%s %s → %s\nRoot #%s · cause #%s\n"
				% [
					event.get("id", 0),
					event.get("name", event.get("kind", "event")),
					event.get("result", ""),
					event.get("root", 0),
					str(event.get("cause", "—"))
				]
			)
	_inspect_body.text = text


func _request_events(thing = "") -> void:
	if not thing.is_empty():
		trace_thing = thing
		trace_root = 0
	var request = {"t": "events", "since": event_since, "limit": 40, "detail": false}
	if trace_root > 0:
		request.merge({"root": trace_root, "since": 0, "limit": 200}, true)
	elif not trace_thing.is_empty():
		request.merge({"thing": trace_thing, "since": 0}, true)
	_send(request)


func _update_feed() -> void:
	if is_instance_valid(_feed):
		_feed.text = RecordFeed.format(records, trace_root, trace_thing)


func _flush_feed() -> void:
	if not _feed_dirty:
		return
	_feed_dirty = false
	if records.size() > 4096:
		var ids = records.keys()
		ids.sort()
		for index in ids.size() - 3072:
			records.erase(ids[index])
	_update_feed()


func _trace_event(event_id) -> void:
	var event = records.get(int(event_id), {})
	trace_root = int(event.get("root", event_id))
	trace_thing = ""
	_request_events()
	_update_feed()


func _recent_record() -> void:
	trace_root = 0
	trace_thing = ""
	_request_events()
	_update_feed()


func _follow(handle: String) -> void:
	if handle.is_empty():
		return
	follow_handle = handle
	if protocol.entities.has(handle):
		_select(handle)
		distance = minf(distance, 11)
		_notify("Following " + _name(handle))
	else:
		_notify("Waiting for the body's stream")


func _focus(handle: String) -> void:
	if not protocol.entities.has(handle):
		return
	follow_handle = ""
	target = Stream.to_view(protocol.entities[handle].position)
	var box = assets.bounds(protocol.entities[handle].asset)
	target += box.get_center()
	distance = clampf(box.size.length() * 1.6, 3, 90)


func _frame_world() -> void:
	follow_handle = ""
	if protocol.entities.is_empty():
		return
	var center = Vector3.ZERO
	for row in protocol.entities.values():
		center += row.position
	target = Stream.to_view(center / protocol.entities.size())
	distance = 28
	orbit_pitch = 0.48
	_framed = true


func _toggle_debug() -> void:
	_debug = not _debug
	world.show_bounds = _debug
	world.show_cells = _debug
	if _debug:
		_send({"t": "debug", "kind": "stats", "around": _server_array(target), "radius": 32})


func _cycle_quality() -> void:
	render_scale = 0.65 if render_scale < 0.6 else (1.0 if render_scale < 0.9 else 0.5)
	_resize_view()
	_notify("Render scale %d%%" % int(render_scale * 100))


func _server_array(position: Vector3) -> Array:
	var value = Stream.to_server(position)
	return [value.x, value.y, value.z]


func _notify(message: String) -> void:
	if _toast:
		_toast.text = message
		_toast.modulate.a = 1.0


func _gateway(
	method: HTTPClient.Method, path: String, body: Dictionary, callback: Callable
) -> void:
	var request = HTTPRequest.new()
	if gateway_url.is_empty():
		callback.call(false, {})
		return
	request.timeout = 4
	add_child(request)
	request.request_completed.connect(_gateway_completed.bind(request, callback))
	var error = request.request(
		gateway_url.trim_suffix("/") + path,
		[
			"Content-Type: application/json",
			"Authorization: Bearer " + (purpose_token if not purpose_token.is_empty() else key)
		],
		method,
		JSON.stringify(body) if method != HTTPClient.METHOD_GET else ""
	)
	if error != OK:
		request.queue_free()
		callback.call(false, {})


func _send_purpose() -> void:
	if not is_instance_valid(_purpose_box) or _purpose_agent.item_count == 0:
		return
	var purpose = _purpose_box.text.strip_edges()
	if purpose.is_empty():
		return
	var agent = _purpose_agent.get_item_metadata(_purpose_agent.selected)
	_purpose_status.text = "Sending purpose to the agent…"
	_gateway(
		HTTPClient.METHOD_POST,
		"/api/purpose",
		{"agent": agent, "purpose": purpose},
		_purpose_completed
	)


func _gateway_completed(
	result, code, _headers, bytes, request: HTTPRequest, callback: Callable
) -> void:
	request.queue_free()
	var parsed = JSON.parse_string(bytes.get_string_from_utf8()) if not bytes.is_empty() else {}
	callback.call(
		result == HTTPRequest.RESULT_SUCCESS and code >= 200 and code < 300,
		parsed if parsed is Dictionary else {}
	)


func _purpose_completed(ok: bool, _reply: Dictionary) -> void:
	if is_instance_valid(_purpose_status):
		_purpose_status.text = (
			"Purpose delivered to the model."
			if ok
			else "Agent service unavailable. Purpose was not delivered."
		)


func _agents_completed(ok: bool, reply: Dictionary) -> void:
	if ok:
		_set_agents(reply.get("agents", []))


func _free_camera() -> void:
	follow_handle = ""
	_notify("Free camera")


func _show_tab(tab: String) -> void:
	_tab = tab
	_sidebar()
	if tab == "record":
		_request_events()


func _close_inspector() -> void:
	selected = ""
	world.selected = ""
	_inspector.hide()


func _request_navigation() -> void:
	_send({"t": "debug", "kind": "nav", "around": _server_array(target), "radius": 32})


func _search_objects(query: String, results: VBoxContainer) -> void:
	for child in results.get_children():
		child.queue_free()
	var found = 0
	for handle in protocol.entities:
		var title = _name(handle)
		if not query.is_empty() and not query.to_lower() in title.to_lower():
			continue
		results.add_child(UITheme.button(title, _select.bind(handle)))
		found += 1
		if found >= 30:
			break


func _ui_diagnostics() -> Dictionary:
	if not browser_diagnostics:
		return {}
	var result = {"tab": _tab, "purposes": {}}
	result.controls = UIProbe.controls([_left_content, _inspector, _entry])
	if _entry.visible:
		result.connection = _entry.diagnostics()
	for id in agents:
		result.purposes[id] = agents[id].get("purpose", "")
	if is_instance_valid(_purpose_box):
		result.purpose = {
			"input": UIProbe.rect(_purpose_box),
			"send": UIProbe.rect(_purpose_send),
			"status": _purpose_status.text
		}
	if is_instance_valid(_feed):
		result.record = {
			"text": _feed.get_parsed_text(),
			"rect": UIProbe.rect(_feed),
			"links": RecordFeed.link_layout(_feed)
		}
	return result


func _process(delta: float) -> void:
	var began = Time.get_ticks_usec()
	network_packets = 0
	var state = socket.get_ready_state()
	if state != WebSocketPeer.STATE_CLOSED:
		socket.poll()
		state = socket.get_ready_state()
	if state == WebSocketPeer.STATE_OPEN:
		if not hello_sent and not key.is_empty():
			hello_sent = true
			socket.send_text(
				JSON.stringify(
					{"t": "hello", "version": 1, "key": key, "mode": mode, "binary": true}
				)
			)
		while socket.get_available_packet_count() > 0:
			network_packets += 1
			var packet = socket.get_packet()
			if socket.was_string_packet():
				var parsed = JSON.parse_string(packet.get_string_from_utf8())
				if parsed is Dictionary:
					_text(parsed)
			elif not protocol.decode(packet):
				_notify(protocol.error)
				socket.close(1002, "Replication decode failed")
				break
		protocol.finish_batch()
		_flush_feed()
	elif state == WebSocketPeer.STATE_CLOSED:
		connected = false
		if not key.is_empty() and Time.get_ticks_msec() > reconnect_ms:
			_connect()
	network_ms = (Time.get_ticks_usec() - began) * 0.001
	if not _framed and protocol.entities.size() > 5:
		_frame_world()
	if not follow_handle.is_empty() and protocol.entities.has(follow_handle):
		var pose = protocol.sample(follow_handle, Time.get_ticks_msec(), false)
		target = target.lerp(pose.origin + Vector3.UP, 1 - exp(-delta * 5))
	var movement = Vector3.ZERO
	var focused = get_viewport().gui_get_focus_owner()
	if not (focused is LineEdit or focused is TextEdit):
		movement.x = (
			float(Input.is_physical_key_pressed(KEY_D))
			- float(Input.is_physical_key_pressed(KEY_A))
		)
		movement.z = (
			float(Input.is_physical_key_pressed(KEY_S))
			- float(Input.is_physical_key_pressed(KEY_W))
		)
		movement.y = (
			float(Input.is_physical_key_pressed(KEY_E))
			- float(Input.is_physical_key_pressed(KEY_Q))
		)
	if movement.length_squared() > 0:
		follow_handle = ""
		target += Basis(Vector3.UP, orbit_yaw) * movement.normalized() * delta * distance * 0.45
	var direction = Vector3(
		sin(orbit_yaw) * cos(orbit_pitch), sin(orbit_pitch), cos(orbit_yaw) * cos(orbit_pitch)
	)
	camera.position = target + direction * distance
	camera.look_at(target, Vector3.UP)
	world.camera_position = camera.position
	if connected and camera.position.distance_to(_last_view) > 3:
		_last_view = camera.position
		_send({"t": "view", "pos": _server_array(camera.position)})
	_clock += delta
	_events_clock += delta
	_gateway_clock += delta
	_toast.modulate.a = maxf(0, _toast.modulate.a - delta * 0.08)
	_empty.visible = protocol.entities.is_empty() and not _entry.visible
	if _clock > 0.5:
		_clock = 0
		_status.text = (
			("LIVE  ·  STEP %d  ·  %d OBJECTS" % [protocol.newest_step, protocol.entities.size()])
			if connected
			else ("ENTER SPECTATOR KEY" if key.is_empty() else "CONNECTING TO THE WORLD")
		)
		_stats.text = (
			"%d FPS  ·  %.1f ms view  ·  %.2f MB assets"
			% [Engine.get_frames_per_second(), world.frame_ms, assets.downloaded_bytes / 1048576.0]
		)
		if not selected.is_empty():
			_update_inspector({})
		if OS.has_feature("web"):
			(
				JavaScriptBridge
				. eval(
					(
						"window.ambergris="
						+ (
							JSON
							. stringify(
								{
									"connected": connected,
									"entities": protocol.entities.size(),
									"residents": agents.size(),
									"assets": assets.cache.size(),
									"step": protocol.newest_step,
									"fps": Engine.get_frames_per_second(),
									"view_ms": world.frame_ms,
									"selected": selected,
									"following": follow_handle,
									"inspection_ready": not inspect_details.is_empty(),
									"records": records.size(),
									"trace_root": trace_root,
									"debug": _debug,
									"groups": world.groups.size(),
									"loading": world.loading_counts(),
									"ui": _ui_diagnostics(),
									"asset_bytes": assets.downloaded_bytes,
									"import_ms": assets.import_ms,
									"network_ms": network_ms,
									"network_packets": network_packets,
									"replication_bytes": protocol.bytes_received,
									"script_ms": (Time.get_ticks_usec() - began) * 0.001,
									"draw_calls":
									Performance.get_monitor(
										Performance.RENDER_TOTAL_DRAW_CALLS_IN_FRAME
									),
									"primitives":
									Performance.get_monitor(
										Performance.RENDER_TOTAL_PRIMITIVES_IN_FRAME
									),
								}
							)
						)
						+ ";"
					),
					true
				)
			)
	if _events_clock > 3 and connected:
		_events_clock = 0
		_request_events()
	if _gateway_clock > 10:
		_gateway_clock = 0
		_gateway(HTTPClient.METHOD_GET, "/api/agents", {}, _agents_completed)


func _unhandled_input(event: InputEvent) -> void:
	if event is InputEventMouseButton:
		if event.button_index == MOUSE_BUTTON_WHEEL_UP:
			distance = maxf(1.2, distance * 0.88)
		if event.button_index == MOUSE_BUTTON_WHEEL_DOWN:
			distance = minf(250, distance / 0.88)
		if event.button_index == MOUSE_BUTTON_LEFT or event.button_index == MOUSE_BUTTON_RIGHT:
			_orbit = event.pressed
			if event.pressed:
				_click_start = event.position
				_dragged = false
			elif event.button_index == MOUSE_BUTTON_LEFT and not _dragged:
				var point = event.position * Vector2(viewport.size) / size
				_select(
					world.pick(
						camera.project_ray_origin(point),
						camera.project_ray_normal(point),
						event.alt_pressed
					)
				)
		if event.button_index == MOUSE_BUTTON_MIDDLE:
			_pan = event.pressed
	elif event is InputEventMouseMotion:
		if event.position.distance_to(_click_start) > 5:
			_dragged = true
		if _orbit:
			orbit_yaw -= event.relative.x * 0.006
			orbit_pitch = clampf(orbit_pitch + event.relative.y * 0.004, 0.04, 1.45)
		if _pan:
			follow_handle = ""
			target += (
				(-camera.basis.x * event.relative.x + camera.basis.y * event.relative.y)
				* distance
				* 0.0015
			)
	elif event is InputEventKey and event.pressed:
		if event.keycode == KEY_H:
			_frame_world()
		if event.keycode == KEY_F and not selected.is_empty():
			_follow(selected)
		if event.keycode == KEY_ESCAPE:
			follow_handle = ""
			get_viewport().gui_release_focus()
