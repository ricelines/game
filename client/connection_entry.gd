extends CenterContainer
## Spectator authentication UI. Credentials remain in memory and are never diagnostic data.

signal submitted(credential: String, view_mode: String)

const UIProbe = preload("res://ui_probe.gd")
const UITheme = preload("res://ui_theme.gd")
var _key: LineEdit
var _mode: OptionButton
var _status: Label
var _connect_button: Button


func setup(initial_mode: String) -> void:
	set_anchors_and_offsets_preset(Control.PRESET_FULL_RECT)
	mouse_filter = Control.MOUSE_FILTER_STOP
	var panel = PanelContainer.new()
	panel.custom_minimum_size = Vector2(420, 0)
	panel.add_theme_stylebox_override("panel", UITheme.style(Color(0.035, 0.065, 0.08, 0.98)))
	add_child(panel)
	var content = VBoxContainer.new()
	content.add_theme_constant_override("separation", 14)
	panel.add_child(content)
	content.add_child(UITheme.label("Enter the world", 25))
	content.add_child(
		UITheme.label(
			"Paste the spectator key supplied by your world host.", 13, Color(0.55, 0.65, 0.66)
		)
	)
	_key = LineEdit.new()
	_key.placeholder_text = "Spectator key"
	_key.secret = true
	_key.custom_minimum_size.y = 46
	_key.add_theme_font_size_override("font_size", 15)
	_key.add_theme_stylebox_override("normal", UITheme.style(Color(0.08, 0.13, 0.14), 8))
	_key.add_theme_stylebox_override("focus", UITheme.style(Color(0.12, 0.2, 0.2), 8))
	content.add_child(_key)
	_mode = OptionButton.new()
	_mode.add_item("Spectator")
	_mode.add_item("Spectator with debug records")
	_mode.select(1 if initial_mode == "debug" else 0)
	content.add_child(_mode)
	_status = UITheme.label("", 12, Color(0.95, 0.69, 0.35))
	_status.autowrap_mode = TextServer.AUTOWRAP_WORD_SMART
	content.add_child(_status)
	_connect_button = UITheme.button("Connect", _submit, true)
	_connect_button.disabled = true
	content.add_child(_connect_button)
	_key.text_changed.connect(
		func(value): _connect_button.disabled = value.strip_edges().is_empty()
	)
	_key.text_submitted.connect(func(_value): _submit())


func open(message = "") -> void:
	_status.text = message
	show()
	_key.grab_focus()


func _submit() -> void:
	var credential = _key.text.strip_edges()
	if credential.is_empty():
		return
	_key.clear()
	_connect_button.disabled = true
	_status.text = "Connecting to the world..."
	submitted.emit(credential, "debug" if _mode.selected == 1 else "watch")


func diagnostics() -> Dictionary:
	return {
		"input": UIProbe.rect(_key),
		"connect": UIProbe.rect(_connect_button),
		"view": UIProbe.rect(_mode),
		"ready": not _connect_button.disabled,
		"status": _status.text
	}
