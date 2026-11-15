extends SceneTree
## Presentation fixture using S7-shaped records; it never starts a game transport.

const App = preload("res://main.gd")
var checks = 0
var failures = 0


func check(value: bool, label: String) -> void:
	checks += 1
	if not value:
		failures += 1
		push_error(label)


func _initialize() -> void:
	var app = App.new()
	app._feed = RichTextLabel.new()
	for id in 5000:
		app._text({"t": "event", "id": id, "kind": "action", "name": "arbitrary.action"})
	check(app._feed.text.is_empty(), "incoming batch does not rebuild UI per event")
	app._flush_feed()
	check(app.records.size() == 3072 and app.event_since == 4999, "bounded recent cache and cursor")
	check(
		app._feed.text.contains("#4999") and not app._feed.text.contains("#0"),
		"recent records rendered"
	)
	app.records = {
		7:
		{
			"id": 7,
			"root": 6,
			"cause": 6,
			"kind": "handler",
			"name": "authored.callback",
			"authors": ["p:9", "p:11"]
		}
	}
	app.trace_root = 6
	app._feed_dirty = true
	app._flush_feed()
	check(
		app._feed.text.contains("authored.callback"),
		"causal trace retains handlers without effects"
	)
	check(
		(
			app._feed.text.contains("root #6 · cause #6")
			and app._feed.text.contains("authors: p:9, p:11")
		),
		"causal feed displays recorded root/cause and actual author set"
	)
	app.records[7].things = ["o:9.1"]
	for id in range(8, 100):
		app.records[id] = {
			"id": id, "kind": "handler", "changed": true, "name": "other.tick", "things": ["o:8.1"]
		}
	app.trace_root = 0
	app.trace_thing = "o:9.1"
	app._update_feed()
	check(
		app._feed.text.contains("authored.callback") and not app._feed.text.contains("other.tick"),
		"object trace retains its actual records despite newer unrelated streamed handlers"
	)
	app.trace_root = 6
	app.trace_thing = ""
	app._update_feed()
	check(
		app._feed.text.contains("CAUSAL CHAIN #6") and app._feed.text.contains("authored.callback"),
		"clicking a root exposes its chain independently of the object filter"
	)
	app.world = preload("res://world_view.gd").new()
	app.world.protocol = app.protocol
	app.assets = preload("res://assets.gd").new()
	app._text(
		JSON.parse_string(
			'{"t":"catalog","terms":[{"id":17,"name":"novel.temperature","unit":"K"}]}'
		)
	)
	check(app.terms.has("17"), "JSON catalog numeric IDs match binary integer state IDs")
	app.protocol.entities["o:9.1"] = {
		"position": Vector3.ZERO, "asset": "fixture", "states": {17: 273.15}
	}
	app.selected = "o:9.1"
	app.inspect_id = 7
	app._inspect_body = RichTextLabel.new()
	app._text(
		{
			"t": "inspection",
			"id": 7.0,
			"h": "o:9.1",
			"elaborated": [{"id": 1, "text": "A weathered edge", "author": "p:3", "created_s": 12}]
		}
	)
	check(
		not app.inspect_details.is_empty(), "canonical native inspection envelope updates selection"
	)
	check(
		(
			app._inspect_body.text.contains("novel.temperature")
			and app._inspect_body.text.contains("K")
		),
		"declared state shows registry name and unit"
	)
	check(
		(
			app._inspect_body.text.contains("ELABORATED DETAILS")
			and app._inspect_body.text.contains("A weathered edge")
			and app._inspect_body.text.contains("p:3")
		),
		"shared detail is visibly separate and attributed"
	)
	app.world.free()
	app.assets.free()
	app._inspect_body.free()
	app._feed.free()
	app.free()
	print("Event feed fixture: %d checks passed" % checks)
	quit(0 if failures == 0 else 1)
