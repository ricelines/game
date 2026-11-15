extends SceneTree
## Real delayed HTTP manifest/GLB import; synthetic entity rows are labelled fixtures.

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
	var arguments = OS.get_cmdline_user_args()
	var cache = Cache.new()
	cache.root_url = arguments[0]
	root.add_child(cache)
	var hash = arguments[1]
	var stream = Stream.new()
	var view = View.new()
	root.add_child(view)
	view.setup(stream, cache)
	stream.entities["o:1.1"] = {
		"asset": hash,
		"states": {},
		"position": Vector3(10, 20, 3),
		"rotation": Quaternion.IDENTITY,
		"samples": [],
		"receive_ms": Time.get_ticks_msec()
	}
	view.invalidate("o:1.1")
	view._process(0.016)
	check(view.draws.has("o:1.1"), "loading entity is visible immediately")
	check(view.draws["o:1.1"].lod == -1, "neutral marker explicitly has no imported LOD")
	check(
		view._display_bounds("o:1.1").size == Vector3.ONE * 0.5,
		"manifest pending uses documented neutral half-metre bounds"
	)
	var deadline = Time.get_ticks_msec() + 15000
	while cache.cache[hash].manifest.is_empty() and Time.get_ticks_msec() < deadline:
		await create_timer(0.02).timeout
	view._process(0.016)
	check(not cache.cache[hash].manifest.is_empty(), "actual HTTP manifest received")
	check(view.draws["o:1.1"].lod == -1, "delayed GLB still displays a loading proxy")
	if arguments.size() > 2:
		cache.ensure(arguments[2], 2)
	var bounds = cache.bounds(hash)
	var proxy = view.draws["o:1.1"].parts[0]
	check(
		proxy.component.transform.origin.is_equal_approx(bounds.get_center()),
		"proxy origin uses actual measured bounds"
	)
	check(
		proxy.component.transform.basis.get_scale().is_equal_approx(bounds.size),
		"proxy size uses actual measured bounds"
	)
	check(
		view.transforms["o:1.1"].origin == Vector3(10, 3, -20),
		"loading proxy uses authoritative transformed world position"
	)
	var old_node = proxy.node
	while cache.available_lod(hash, 2) < 0 and Time.get_ticks_msec() < deadline:
		await create_timer(0.02).timeout
	view._process(0.016)
	check(view.draws["o:1.1"].lod >= 0, "real runtime GLB replaces the temporary box")
	check(
		view.draws["o:1.1"].parts[0].component.mesh != view._proxy_mesh,
		"replacement uses imported geometry rather than placeholder triangles"
	)
	await process_frame
	check(not is_instance_valid(old_node), "temporary proxy is removed after replacement")
	if arguments.size() > 2:
		var peer = arguments[2]
		cache.ensure(peer, 2)
		while cache.available_lod(peer, 2) < 0 and Time.get_ticks_msec() < deadline:
			await create_timer(0.02).timeout
		check(cache.available_lod(peer, 2) == 2, "distinct authored peer receives shared geometry")
		check(
			cache.cache[peer].meshes[2][0].mesh == cache.cache[hash].meshes[2][0].mesh,
			"same trusted geometry imports once across distinct immutable manifests"
		)
	print(
		JSON.stringify(
			{
				"fixture": "delayed real HTTP GLB loading proxy",
				"checks": checks,
				"failures": failures,
				"downloaded_bytes": cache.downloaded_bytes
			}
		)
	)
	quit(0 if failures == 0 else 1)
