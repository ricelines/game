extends SceneTree
## Labelled renderer fixture: real baked mesh, synthetic wire rows, no server/game claim.

const Stream = preload("res://protocol.gd")
const Cache = preload("res://assets.gd")
const View = preload("res://world_view.gd")
var checks = 0
var failures = 0


func check(value: bool, label: String) -> void:
	checks += 1
	if not value:
		failures += 1
		push_error(label)


func _initialize() -> void:
	call_deferred("run")


func run() -> void:
	var args = OS.get_cmdline_user_args()
	if args.is_empty():
		push_error("Provide an actual baked asset directory after --")
		quit(1)
		return
	var directory = args[0]
	var manifest = JSON.parse_string(FileAccess.get_file_as_string(directory + "/manifest.json"))
	var hash = str(manifest.hash)
	var cache = Cache.new()
	root.add_child(cache)
	var lods = {}
	for lod in 3:
		var document = GLTFDocument.new()
		var state = GLTFState.new()
		check(
			(
				document.append_from_buffer(
					FileAccess.get_file_as_bytes(directory + "/lod%d.glb" % lod), "", state
				)
				== OK
			),
			"actual baked LOD%d import" % lod
		)
		var scene = document.generate_scene(state)
		var components: Array = []
		cache._collect(scene, Transform3D.IDENTITY, components)
		scene.free()
		lods[lod] = components
	var meshes = lods[2]
	cache.cache[hash] = {"status": "ready", "manifest": manifest, "meshes": lods}
	check(not meshes.is_empty(), "real baked triangles present")
	var stream = Stream.new()
	var view = View.new()
	root.add_child(view)
	view.setup(stream, cache)
	view.camera_position = Vector3(0, 10000, 0)
	view.materials[7] = {"color": [0.6, 0.4, 0.2, 1], "roughness": 0.6}
	view.terms["arbitrary.pigment"] = {"id": 2}
	view.terms["arbitrary.alpha"] = {"id": 3}
	view.models[9] = {
		"bindings":
		{
			"render":
			[
				{
					"term": "arbitrary.pigment",
					"property": "color",
					"colors": [[0, 0, 0, 1], [1, 0.5, 0, 1]]
				},
				{"term": "arbitrary.alpha", "property": "opacity"}
			]
		}
	}
	for index in 1000:
		var handle = "o:%d.1" % (index + 1)
		stream.entities[handle] = {
			"asset": hash,
			"states": {2: 0.5, 3: 1.0},
			"position": Vector3(index % 40, index / 40, 0),
			"rotation": Quaternion.IDENTITY,
			"samples": [],
			"receive_ms": Time.get_ticks_msec()
		}
		view.metadata[handle] = {"model": 9, "material": 7}
		view.invalidate(handle)
	var began = Time.get_ticks_usec()
	view._process(0.016)
	check(view.draws.size() == 1000, "all ordinary objects rendered")
	check(view.groups.size() == meshes.size(), "immutable meshes batch across instances")
	check(
		view._visual("o:1.1", meshes[0].name).tint.is_equal_approx(Color(0.5, 0.25, 0, 1)),
		"arbitrary term controls authored color"
	)
	stream.entities["o:1.1"].states[3] = 0.4
	view.invalidate("o:1.1")
	view._process(0.016)
	view._process(0.016)
	check(
		view.draws["o:1.1"].parts[0].transparent, "opacity crossing rebuilds correct render class"
	)
	view.remove("o:2.1")
	check(view.draws.size() == 999, "ordinary deletion removes only one object")
	var remaining = view.groups.values()[0]
	check(remaining.handles.size() == 998, "batch swap-removal preserves slots")
	check(view.draws["o:3.1"].lod == 2, "distant objects use actual low-detail geometry")
	view.selected = "o:3.1"
	view._process(0.5)
	check(view.draws["o:3.1"].lod == 0, "selected object upgrades to actual full-detail geometry")
	stream.entities["o:4.1"].states[2] = 0.8
	view.invalidate("o:4.1")
	view._process(0.016)
	var part = view.draws["o:4.1"].parts[0]
	check(
		is_equal_approx(part.visual.tint.r, 0.8),
		"cached drawing updates its submitted color after an authoritative state change"
	)
	for index in 2:
		var handle = "o:%d.1" % (1001 + index)
		var model = 10 + index
		view.models[model] = {
			"bindings":
			{
				"zone":
				{
					"ambient": 120 if index == 0 else 2,
					"priority": index,
					"shape": {"kind": "box", "size": [10, 10, 10], "offset": [0, 0, 0]},
				}
			}
		}
		stream.entities[handle] = {
			"asset": hash,
			"states": {},
			"position": Vector3.ZERO,
			"rotation": Quaternion.IDENTITY,
			"samples": [],
			"receive_ms": Time.get_ticks_msec()
		}
		view.metadata[handle] = {"model": model, "material": 7}
		view.invalidate(handle)
	view._process(0.016)
	check(
		is_equal_approx(view._ambient(Vector3.ZERO), 0.2),
		"highest containing authored zone controls exposure"
	)
	check(
		view._ambient(Vector3(30, 0, 0)) < 0.2,
		"zone influence respects its actual transformed volume"
	)
	print(
		JSON.stringify(
			{
				"fixture": "real baked GLB renderer, synthetic rows",
				"checks": checks,
				"failures": failures,
				"objects": 1000,
				"total_ms": (Time.get_ticks_usec() - began) * 0.001,
				"asset": hash
			}
		)
	)
	quit(0 if failures == 0 else 1)
