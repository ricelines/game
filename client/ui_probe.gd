extends RefCounted
## Optional browser diagnostics: measured controls, with no alternate input path.


static func rect(control: Control) -> Array:
	var bounds = control.get_global_rect()
	return [bounds.position.x, bounds.position.y, bounds.size.x, bounds.size.y]


static func controls(roots: Array) -> Array:
	var rows = []
	for root in roots:
		_collect(root, rows)
	return rows


static func _collect(node: Node, rows: Array) -> void:
	if node is Control and not node.is_visible_in_tree():
		return
	if node is Button or node is LineEdit:
		rows.append(
			{
				"kind": "button" if node is Button else "input",
				"text": node.text if node is Button else node.placeholder_text,
				"rect": rect(node)
			}
		)
	for child in node.get_children():
		_collect(child, rows)
