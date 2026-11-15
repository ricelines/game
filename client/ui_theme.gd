extends RefCounted
## Shared spectator colors and control styles.

const PAPER = Color(0.93, 0.91, 0.85)
const AMBER = Color(0.95, 0.69, 0.35)


static func style(color: Color, radius = 14) -> StyleBoxFlat:
	var result = StyleBoxFlat.new()
	result.bg_color = color
	result.corner_radius_top_left = radius
	result.corner_radius_top_right = radius
	result.corner_radius_bottom_left = radius
	result.corner_radius_bottom_right = radius
	result.border_width_left = 1
	result.border_width_top = 1
	result.border_width_right = 1
	result.border_width_bottom = 1
	result.border_color = Color(0.65, 0.75, 0.72, 0.16)
	result.content_margin_left = 18
	result.content_margin_right = 18
	result.content_margin_top = 15
	result.content_margin_bottom = 15
	return result


static func label(text: String, font_size = 14, color = PAPER) -> Label:
	var result = Label.new()
	result.text = text
	result.add_theme_font_size_override("font_size", font_size)
	result.add_theme_color_override("font_color", color)
	return result


static func button(text: String, action: Callable, accent = false) -> Button:
	var result = Button.new()
	result.text = text
	result.focus_mode = Control.FOCUS_NONE
	result.mouse_default_cursor_shape = Control.CURSOR_POINTING_HAND
	result.add_theme_font_size_override("font_size", 13)
	result.add_theme_color_override("font_color", Color(0.08, 0.1, 0.11) if accent else PAPER)
	result.add_theme_stylebox_override(
		"normal", style(AMBER if accent else Color(0.12, 0.18, 0.19, 0.85), 8)
	)
	result.add_theme_stylebox_override(
		"hover", style(Color(1, 0.79, 0.49) if accent else Color(0.2, 0.28, 0.28, 0.95), 8)
	)
	result.add_theme_stylebox_override("pressed", style(Color(0.6, 0.42, 0.22), 8))
	result.add_theme_stylebox_override("disabled", style(Color(0.09, 0.13, 0.14), 8))
	result.add_theme_color_override("font_disabled_color", Color(0.55, 0.65, 0.66))
	result.pressed.connect(action)
	return result
