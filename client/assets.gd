extends Node
## Cache shared immutable assets. Loading proxies are separate from imported baked GLBs.

signal loaded(hash: String)
signal manifest_loaded(hash: String)
signal failed(hash: String, reason: String)

var root_url = ""
var cache: Dictionary = {}
var downloaded_bytes = 0
var import_ms = 0.0
var _queue: Array = []
var _active = 0


func ensure(hash: String, lod = 2) -> void:
	if hash.is_empty() or root_url.is_empty():
		return
	if not cache.has(hash):
		cache[hash] = {
			"status": "loading", "meshes": {}, "manifest": {}, "lod_status": {}, "wanted": {}
		}
		_queue.append({"hash": hash, "stage": "manifest"})
	var row = cache[hash]
	row.wanted[lod] = true
	if not row.manifest.is_empty():
		_request_lod(hash, lod)
	elif row.status == "failed" and Time.get_ticks_msec() - row.retry_ms >= 10000:
		row.status = "loading"
		_queue.append({"hash": hash, "stage": "manifest"})
	_pump()


func _request_lod(hash: String, lod: int) -> void:
	var row = cache[hash]
	if row.meshes.has(lod):
		return
	var status = row.lod_status.get(lod, {})
	if not status.is_empty():
		if status.status != "failed" or Time.get_ticks_msec() - status.retry_ms < 10000:
			return
	row.lod_status[lod] = {"status": "loading"}
	_queue.append({"hash": hash, "stage": "glb", "lod": lod})


func available_lod(hash: String, wanted: int) -> int:
	var meshes = cache.get(hash, {}).get("meshes", {})
	var best = -1
	for lod in [2, 1, 0]:
		if meshes.has(lod) and (best < 0 or absi(lod - wanted) < absi(best - wanted)):
			best = lod
	return best


func _pump() -> void:
	while _active < 4 and not _queue.is_empty():
		var item = _queue.pop_front()
		var request = HTTPRequest.new()
		request.timeout = 20.0
		add_child(request)
		request.request_completed.connect(_completed.bind(item, request))
		var file = "manifest.json" if item.stage == "manifest" else "lod%d.glb" % item.lod
		_active += 1
		var error = request.request(root_url.trim_suffix("/") + "/" + item.hash + "/" + file)
		if error != OK:
			_active -= 1
			request.queue_free()
			_fail(item, error_string(error))


func _fail(item: Dictionary, reason: String) -> void:
	var row = cache[item.hash]
	if item.stage == "glb":
		row.lod_status[item.lod] = {"status": "failed", "retry_ms": Time.get_ticks_msec()}
	row.status = "failed" if row.meshes.is_empty() else "ready"
	row.retry_ms = Time.get_ticks_msec()
	row.error = reason
	failed.emit(item.hash, reason)


func _completed(
	result: int,
	code: int,
	_headers: PackedStringArray,
	body: PackedByteArray,
	item: Dictionary,
	request: HTTPRequest
) -> void:
	_active -= 1
	request.queue_free()
	downloaded_bytes += body.size()
	if result != HTTPRequest.RESULT_SUCCESS or code != 200:
		_fail(item, "HTTP %d / %s" % [code, error_string(result)])
	elif item.stage == "manifest":
		var manifest = JSON.parse_string(body.get_string_from_utf8())
		if manifest is Dictionary:
			cache[item.hash].manifest = manifest
			manifest_loaded.emit(item.hash)
			for lod in cache[item.hash].wanted:
				_request_lod(item.hash, lod)
		else:
			_fail(item, "Invalid asset manifest")
	else:
		var began = Time.get_ticks_usec()
		var document = GLTFDocument.new()
		var state = GLTFState.new()
		var error = document.append_from_buffer(body, "", state)
		if error == OK:
			var scene = document.generate_scene(state)
			var meshes: Array = []
			_collect(scene, Transform3D.IDENTITY, meshes)
			scene.free()
			cache[item.hash].meshes[item.lod] = meshes
			cache[item.hash].lod_status[item.lod] = {"status": "ready"}
			cache[item.hash].status = "ready"
			import_ms += (Time.get_ticks_usec() - began) * 0.001
			loaded.emit(item.hash)
		else:
			_fail(item, "GLB import: " + error_string(error))
	_pump()


func _collect(node: Node, parent: Transform3D, meshes: Array) -> void:
	var transform = parent
	if node is Node3D:
		transform = parent * node.transform
	if node is MeshInstance3D and node.mesh:
		meshes.append({"name": str(node.name), "mesh": node.mesh, "transform": transform})
	for child in node.get_children():
		_collect(child, transform, meshes)


func bounds(hash: String) -> AABB:
	if not cache.has(hash):
		return AABB()
	var manifest = cache[hash].manifest
	if not manifest.has("bounds"):
		return AABB()
	var low = manifest.bounds.min
	var high = manifest.bounds.max
	return AABB(
		Vector3(low[0], low[2], -high[1]),
		Vector3(high[0] - low[0], high[2] - low[2], high[1] - low[1])
	)
