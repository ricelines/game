extends SceneTree
## Actual RichTextLabel shaping, including wrapped records and link click positions.

const Feed = preload("res://record_feed.gd")


func _initialize() -> void:
	call_deferred("run")


func run() -> void:
	var label = RichTextLabel.new()
	label.bbcode_enabled = true
	label.position = Vector2(40, 40)
	label.size = Vector2(250, 600)
	label.add_theme_font_size_override("normal_font_size", 13)
	root.add_child(label)
	label.text = (
		Feed
		. format(
			{
				101:
				{
					"id": 101,
					"root": 101,
					"cause": null,
					"kind": "action",
					"name": "arbitrary.action",
					"authors": ["p:9"]
				},
				102:
				{
					"id": 102,
					"root": 101,
					"cause": 101,
					"kind": "handler",
					"changed": true,
					"name": "A long arbitrary handler description that wraps into several lines",
					"authors": ["p:9", "p:11"]
				},
			},
			0
		)
	)
	await process_frame
	await process_frame
	var links = Feed.link_layout(label)
	var valid = links.size() == 2 and links[0].id == 102 and links[1].id == 101
	for link in links:
		valid = valid and Rect2(40, 40, 250, 600).has_point(Vector2(link.point[0], link.point[1]))
	print(
		JSON.stringify(
			{"fixture": "actual RichTextLabel link layout", "passed": valid, "links": links}
		)
	)
	quit(0 if valid else 1)
