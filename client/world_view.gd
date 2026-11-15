extends Node3D
## Presentation only: immutable meshes, authoritative transforms, declared value bindings.

const Protocol = preload("res://protocol.gd")
const PROXY_MATERIAL = {"color": [0.4, 0.7, 0.8, 0.25], "roughness": 1.0}
var protocol
var assets
var models: Dictionary = {}
var materials: Dictionary = {}
var metadata: Dictionary = {}
var terms: Dictionary = {}
var world_ambient_lux = 1.0
var exposure_lux = 8.0
var camera_position = Vector3(0, 10, 20)
var draws: Dictionary = {}
var groups: Dictionary = {}
var transforms: Dictionary = {}
var headings: Dictionary = {}
var dirty: Dictionary = {}
var moving: Dictionary = {}
var pending: Dictionary = {}
var zones: Dictionary = {}
var selected = ""
var show_bounds = false
var show_cells = false
var debug_segments: Array = []
var frame_ms = 0.0
var _zone_order: Array = []
var _zone_transforms: Dictionary = {}
var _opaque_shader: Shader
var _transparent_shader: Shader
var _lines: MeshInstance3D
var _line_material: StandardMaterial3D
var _line_timer = 0.0
var _lod_timer = 0.0
var _proxy_mesh: BoxMesh


func setup(stream, asset_cache) -> void:
	protocol = stream
	assets = asset_cache
	protocol.entity_changed.connect(invalidate)
	protocol.entity_removed.connect(forget)
	assets.loaded.connect(_asset_loaded)
	assets.manifest_loaded.connect(_asset_loaded)
	_proxy_mesh = BoxMesh.new()
	_proxy_mesh.size = Vector3.ONE
	_opaque_shader = _shader(false)
	_transparent_shader = _shader(true)
	_lines = MeshInstance3D.new()
	add_child(_lines)
	_line_material = StandardMaterial3D.new()
	_line_material.shading_mode = BaseMaterial3D.SHADING_MODE_UNSHADED
	_line_material.vertex_color_use_as_albedo = true
	_line_material.transparency = BaseMaterial3D.TRANSPARENCY_ALPHA
	_line_material.no_depth_test = true
	_line_material.render_priority = 10
	_lines.material_override = _line_material


func _shader(transparent: bool) -> Shader:
	var shader = Shader.new()
	shader.code = (
		"""shader_type spatial;
render_mode cull_back, diffuse_lambert, specular_disabled, vertex_lighting;
uniform vec4 base_color : source_color = vec4(1.0);
uniform float surface_roughness = 0.7;
uniform float surface_metallic = 0.0;
uniform bool instanced = true;
uniform vec4 object_tint : source_color = vec4(1.0);
uniform vec4 object_data = vec4(0.0);
varying vec4 instance_tint;
varying vec4 instance_data;
void vertex(){instance_tint = vec4(INSTANCE_CUSTOM.rgb,1.0); instance_data = INSTANCE_CUSTOM;}
void fragment(){
 vec4 tint = instanced ? instance_tint : object_tint;
 vec4 state = instanced ? instance_data : object_data;
 ALBEDO = base_color.rgb * tint.rgb;
 ROUGHNESS = surface_roughness;
 METALLIC = surface_metallic;
 EMISSION = ALBEDO * state.a;
 %s
}
"""
		% ("ALPHA = base_color.a * tint.a;" if transparent else "")
	)
	return shader


func invalidate(handle: String) -> void:
	dirty[handle] = true
	if not draws.has(handle):
		pending[handle] = true
	if zones.has(handle):
		for member in draws:
			dirty[member] = true
	if protocol.entities.has(handle):
		var samples = protocol.entities[handle].samples
		if samples.size() > 1:
			moving[handle] = true


func invalidate_model_data() -> void:
	for handle in protocol.entities:
		remove(handle)
		pending[handle] = true


func _asset_loaded(hash: String) -> void:
	for handle in protocol.entities:
		if protocol.entities[handle].asset == hash:
			var lod = assets.available_lod(hash, _desired_lod(handle))
			if not draws.has(handle) or draws[handle].lod != lod or lod < 0:
				remove(handle)
				pending[handle] = true


func loading_counts() -> Dictionary:
	var counts = {"neutral": 0, "measured": 0}
	for handle in draws:
		if draws[handle].lod < 0:
			var hash = draws[handle].asset
			var measured = not assets.cache.get(hash, {}).get("manifest", {}).is_empty()
			counts["measured" if measured else "neutral"] += 1
	return counts


func _desired_lod(handle: String) -> int:
	if handle == selected:
		return 0
	var row = protocol.entities[handle]
	var box = assets.bounds(row.asset)
	var radius = maxf(0.1, box.size.length() * 0.5)
	var center = Protocol.to_view(row.position) + box.get_center()
	var relative_distance = camera_position.distance_to(center) / radius
	return 2 if relative_distance > 12 else (1 if relative_distance > 3 else 0)


func _check_lods() -> void:
	for handle in draws.keys():
		var hash = protocol.entities[handle].asset
		var desired = _desired_lod(handle)
		assets.ensure(hash, desired)
		var available = assets.available_lod(hash, desired)
		if available >= 0 and draws[handle].lod != available:
			remove(handle)
			pending[handle] = true


func _model(handle: String) -> Dictionary:
	var object = metadata.get(handle, {})
	return models.get(int(object.get("model", 0)), {})


func _material(handle: String) -> Dictionary:
	var object = metadata.get(handle, {})
	return materials.get(int(object.get("material", _model(handle).get("material", 0))), {})


func _bindings(handle: String) -> Dictionary:
	var row = protocol.entities[handle]
	if draws.has(handle) and draws[handle].asset == row.asset:
		return draws[handle].binding
	var result = assets.cache.get(row.asset, {}).get("manifest", {}).get("bindings", {})
	var override = _model(handle).get("bindings", {})
	return override if not override.is_empty() else result


func _value(handle: String, reference, fallback = 0.0) -> float:
	if reference is float or reference is int:
		return float(reference)
	var term = int(terms.get(str(reference), {}).get("id", -1))
	if str(reference).is_valid_int():
		term = int(reference)
	return float(protocol.entities[handle].states.get(term, fallback))


func _color(values, fallback = Color.WHITE) -> Color:
	if not values is Array or values.size() < 3:
		return fallback
	return Color(values[0], values[1], values[2], values[3] if values.size() > 3 else 1.0)


func _visual(handle: String, node_name: String) -> Dictionary:
	var tint = Color.WHITE
	var emission = 0.0
	var visible = true
	for binding in _bindings(handle).get("render", []):
		if binding.has("node") and binding.node != node_name:
			continue
		var value = (
			_value(handle, binding.get("term", 0.0)) * float(binding.get("scale", 1))
			+ float(binding.get("offset", 0))
		)
		match str(binding.get("property", "")):
			"color":
				var colors = binding.get("colors", [[1, 1, 1, 1], [1, 1, 1, 1]])
				var interval = binding.get("range", [0, 1])
				var weight = clampf(
					(
						(value - float(interval[0]))
						/ maxf(0.000001, float(interval[1]) - float(interval[0]))
					),
					0,
					1
				)
				tint = _color(colors[0]).lerp(_color(colors[-1]), weight)
			"emission":
				emission = maxf(0, value)
			"visibility":
				visible = value > 0
			"opacity":
				tint.a *= clampf(value, 0, 1)
	return {"tint": tint, "emission": emission, "visible": visible}


func _make_material(
	handle: String, transparent: bool, instanced: bool, proxy: bool = false
) -> ShaderMaterial:
	var result = ShaderMaterial.new()
	result.shader = _transparent_shader if transparent else _opaque_shader
	var source = PROXY_MATERIAL if proxy else _material(handle)
	result.set_shader_parameter("base_color", _color(source.get("color", [0.72, 0.75, 0.76, 1])))
	result.set_shader_parameter("surface_roughness", float(source.get("roughness", 0.7)))
	result.set_shader_parameter("surface_metallic", float(source.get("metallic", 0)))
	result.set_shader_parameter("instanced", instanced)
	return result


func _display_bounds(handle: String) -> AABB:
	var row = protocol.entities[handle]
	if assets.cache.get(row.asset, {}).get("manifest", {}).is_empty():
		# Neutral half-metre marker, never presented as a measured dimension.
		return AABB(Vector3(-0.25, 0, -0.25), Vector3(0.5, 0.5, 0.5))
	return assets.bounds(row.asset)


func _create(handle: String) -> bool:
	if not protocol.entities.has(handle):
		return true
	var row = protocol.entities[handle]
	var wanted = _desired_lod(handle)
	assets.ensure(row.asset, wanted)
	var lod = assets.available_lod(row.asset, wanted)
	var box = _display_bounds(handle)
	var meshes: Array
	if lod < 0:
		meshes = [
			{
				"name": "",
				"mesh": _proxy_mesh,
				"transform": Transform3D(Basis.IDENTITY.scaled(box.size), box.get_center())
			}
		]
	else:
		meshes = assets.cache[row.asset].meshes[lod]
	var material = PROXY_MATERIAL if lod < 0 else _material(handle)
	var base_alpha = _color(material.get("color", [1, 1, 1, 1])).a
	var probe = box.get_center()
	var drawing = {
		"parts": [],
		"asset": row.asset,
		"material": material,
		"light": null,
		"lod": lod,
		"ambient_probe": probe
	}
	var binding = _bindings(handle)
	drawing.binding = binding
	drawing.states = row.states.duplicate()
	if binding.has("zone"):
		var zone = binding.zone
		var shape = zone.get("shape", {})
		var offset = shape.get("offset", [0, 0, 0])
		var size = shape.get("size", [0, 0, 0])
		var half = Vector3(size[0], size[2], size[1]) * 0.5
		if shape.get("kind", "") == "sphere":
			half = Vector3.ONE * float(shape.get("radius", 0))
		zones[handle] = {
			"ambient": zone.get("ambient", 0.0),
			"priority": float(zone.get("priority", 0)),
			"kind": str(shape.get("kind", "")),
			"offset": Protocol.to_view(Vector3(offset[0], offset[1], offset[2])),
			"half": half,
			"lux": 0.0,
		}
		_zone_order.append(handle)
		_zone_order.sort_custom(
			func(left, right):
				if zones[left].priority != zones[right].priority:
					return zones[left].priority > zones[right].priority
				var a = str(left).trim_prefix("o:").split(".")
				var b = str(right).trim_prefix("o:").split(".")
				return int(a[0]) < int(b[0]) if a[1] == b[1] else int(a[1]) < int(b[1])
		)
		for member in draws:
			dirty[member] = true
	for index in meshes.size():
		var component = meshes[index]
		var visual = _visual(handle, component.name)
		var transparent = base_alpha * visual.tint.a < 0.995
		if transparent:
			var mesh_node = MeshInstance3D.new()
			mesh_node.mesh = component.mesh
			mesh_node.material_override = _make_material(handle, true, false, lod < 0)
			mesh_node.cast_shadow = GeometryInstance3D.SHADOW_CASTING_SETTING_OFF
			add_child(mesh_node)
			drawing.parts.append(
				{"node": mesh_node, "component": component, "transparent": true, "visual": visual}
			)
		else:
			var material_id = int(
				metadata.get(handle, {}).get("material", _model(handle).get("material", 0))
			)
			var geometry = assets.cache[row.asset].manifest.geometry_hash
			var key = geometry + ":" + str(lod) + ":" + str(material_id) + ":" + str(index)
			if not groups.has(key):
				var multi = MultiMesh.new()
				multi.transform_format = MultiMesh.TRANSFORM_3D
				multi.use_custom_data = true
				multi.mesh = component.mesh
				var node = MultiMeshInstance3D.new()
				node.multimesh = multi
				node.material_override = _make_material(handle, false, true)
				node.cast_shadow = GeometryInstance3D.SHADOW_CASTING_SETTING_OFF
				add_child(node)
				groups[key] = {"node": node, "multi": multi, "handles": [], "capacity": 0}
			var group = groups[key]
			group.handles.append(handle)
			if group.handles.size() > group.capacity:
				group.capacity = maxi(8, group.capacity * 2)
				group.multi.instance_count = group.capacity
				for member in group.handles:
					dirty[member] = true
			group.multi.visible_instance_count = group.handles.size()
			drawing.parts.append(
				{
					"group": key,
					"slot": group.handles.size() - 1,
					"component": component,
					"transparent": false,
					"visual": visual
				}
			)
	if binding.has("light"):
		var lamp = OmniLight3D.new()
		lamp.light_color = _color(binding.light.get("color", [1, 0.8, 0.55]))
		lamp.omni_range = float(binding.light.get("range", 12))
		lamp.omni_attenuation = 1.3
		lamp.shadow_enabled = false
		add_child(lamp)
		drawing.light = lamp
	draws[handle] = drawing
	dirty[handle] = true
	return true


func forget(handle: String) -> void:
	remove(handle)
	headings.erase(handle)


func remove(handle: String) -> void:
	if draws.has(handle):
		for part in draws[handle].parts:
			if part.transparent:
				part.node.queue_free()
			else:
				var group = groups[part.group]
				var last = group.handles.pop_back()
				if last != handle:
					group.handles[part.slot] = last
					for other in draws[last].parts:
						if other.get("group", "") == part.group:
							other.slot = part.slot
					dirty[last] = true
				group.multi.visible_instance_count = group.handles.size()
				if group.handles.is_empty():
					group.node.queue_free()
					groups.erase(part.group)
		if draws[handle].light:
			draws[handle].light.queue_free()
		draws.erase(handle)
	transforms.erase(handle)
	if zones.erase(handle):
		_zone_order.erase(handle)
		_zone_transforms.erase(handle)
		for member in draws:
			dirty[member] = true
	dirty.erase(handle)
	moving.erase(handle)
	pending.erase(handle)


func _ambient(position: Vector3) -> float:
	var lux = world_ambient_lux
	for handle in _zone_order:
		if not _zone_transforms.has(handle):
			continue
		var zone = zones[handle]
		var box = zone.world_box
		if not box.has_point(position):
			continue
		var local = _zone_transforms[handle] * position - zone.offset
		var inside = false
		if zone.kind == "box":
			var half = zone.half
			inside = (
				absf(local.x) <= half.x and absf(local.y) <= half.y and absf(local.z) <= half.z
			)
		elif zone.kind == "sphere":
			inside = local.length_squared() <= zone.half.x * zone.half.x
		if inside:
			lux = zone.lux
			break
	return clampf(lux / (maxf(0, lux) + exposure_lux), 0.015, 1.0)


func _process(delta: float) -> void:
	if not protocol:
		return
	var began = Time.get_ticks_usec()
	_lod_timer += delta
	if _lod_timer >= 0.4:
		_lod_timer = 0
		_check_lods()
	for handle in pending.keys():
		if _create(handle):
			pending.erase(handle)
	var now = Time.get_ticks_msec()
	for handle in _zone_order:
		var pose = protocol.sample(handle, now)
		var zone = zones[handle]
		_zone_transforms[handle] = pose.affine_inverse()
		zone.world_box = pose * AABB(zone.offset - zone.half, zone.half * 2)
		zone.lux = _value(handle, zone.ambient)
	var update = dirty.duplicate()
	for handle in moving:
		update[handle] = true
	for handle in update:
		if not draws.has(handle) or not protocol.entities.has(handle):
			continue
		var binding = _bindings(handle)
		var face_motion = binding.get("face_motion", false)
		var transform = protocol.sample(handle, now, not face_motion)
		if face_motion:
			var samples = protocol.entities[handle].samples
			if samples.size() >= 2:
				var direction = Protocol.to_view(samples[-1].position - samples[-2].position)
				if direction.length_squared() > 0.00001:
					var up = (
						Vector3.UP if absf(direction.normalized().y) < 0.98 else Vector3.FORWARD
					)
					headings[handle] = Basis.looking_at(direction, up)
			if headings.has(handle):
				transform.basis = headings[handle]
		transforms[handle] = transform
		var row = protocol.entities[handle]
		if draws[handle].asset != row.asset:
			remove(handle)
			pending[handle] = true
			continue
		var changed_alpha = false
		if draws[handle].states != row.states:
			draws[handle].states = row.states.duplicate()
			var base_alpha = _color(draws[handle].material.get("color", [1, 1, 1, 1])).a
			for part in draws[handle].parts:
				part.visual = _visual(handle, part.component.name)
				if (base_alpha * part.visual.tint.a < 0.995) != part.transparent:
					changed_alpha = true
					break
		if changed_alpha:
			remove(handle)
			pending[handle] = true
			continue
		var ambient = _ambient(transform * draws[handle].ambient_probe)
		for part in draws[handle].parts:
			var visual = part.visual
			var pose = transform * part.component.transform
			if not visual.visible:
				pose.basis = Basis.IDENTITY.scaled(Vector3.ZERO)
			var custom = Color(
				visual.tint.r, visual.tint.g, visual.tint.b, visual.emission + ambient
			)
			if part.transparent:
				part.node.transform = pose
				part.node.material_override.set_shader_parameter("object_tint", visual.tint)
				part.node.material_override.set_shader_parameter("object_data", custom)
			else:
				var multi = groups[part.group].multi
				multi.set_instance_transform(part.slot, pose)
				multi.set_instance_custom_data(part.slot, custom)
		if draws[handle].light:
			var light = binding.light
			var site = light.get("site", [0, 0, 0])
			draws[handle].light.position = (
				transform * Protocol.to_view(Vector3(site[0], site[1], site[2]))
			)
			draws[handle].light.light_energy = (
				maxf(0, _value(handle, light.get("intensity", 0.0))) / 100.0
			)
		if moving.has(handle) and now - protocol.entities[handle].get("receive_ms", now) > 350:
			moving.erase(handle)
	dirty.clear()
	_line_timer += delta
	if _line_timer > 0.15:
		_line_timer = 0
		_debug_lines()
	frame_ms = (Time.get_ticks_usec() - began) * 0.001


func _debug_lines() -> void:
	var mesh = ImmediateMesh.new()
	mesh.surface_begin(Mesh.PRIMITIVE_LINES)
	var count = 0
	for handle in transforms:
		if handle != selected and not show_bounds:
			continue
		var box = _display_bounds(handle)
		if box.size.length_squared() == 0:
			continue
		mesh.surface_set_color(
			Color(1, 0.76, 0.35, 0.95) if handle == selected else Color(0.3, 0.8, 0.8, 0.35)
		)
		for edge in [
			[0, 1],
			[0, 2],
			[0, 4],
			[1, 3],
			[1, 5],
			[2, 3],
			[2, 6],
			[3, 7],
			[4, 5],
			[4, 6],
			[5, 7],
			[6, 7]
		]:
			mesh.surface_add_vertex(transforms[handle] * box.get_endpoint(edge[0]))
			mesh.surface_add_vertex(transforms[handle] * box.get_endpoint(edge[1]))
			count += 2
	if show_cells:
		mesh.surface_set_color(Color(0.3, 0.7, 0.8, 0.25))
		for cell in protocol.cells:
			if cell.x == 32767:
				continue
			var x = cell.x * protocol.cell_m
			var y = cell.y * protocol.cell_m
			for edge in [
				[Vector3(x, y, 0.025), Vector3(x + protocol.cell_m, y, 0.025)],
				[Vector3(x, y, 0.025), Vector3(x, y + protocol.cell_m, 0.025)]
			]:
				mesh.surface_add_vertex(Protocol.to_view(edge[0]))
				mesh.surface_add_vertex(Protocol.to_view(edge[1]))
				count += 2
	mesh.surface_set_color(Color(0.4, 0.9, 0.6, 0.7))
	for line in debug_segments:
		for point in line:
			mesh.surface_add_vertex(Protocol.to_view(Vector3(point[0], point[1], point[2])))
			count += 1
	if count == 0:
		mesh.surface_add_vertex(Vector3.ZERO)
		mesh.surface_add_vertex(Vector3.ZERO)
	mesh.surface_end()
	_lines.mesh = mesh


func pick(origin: Vector3, direction: Vector3, include_transparent: bool) -> String:
	var closest = INF
	var result = ""
	for handle in transforms:
		if not include_transparent and _color(_material(handle).get("color", [1, 1, 1, 1])).a < 0.5:
			continue
		var inverse = transforms[handle].affine_inverse()
		var hit = _display_bounds(handle).intersects_ray(
			inverse * origin, inverse.basis * direction
		)
		if hit is Vector3:
			var distance = origin.distance_squared_to(transforms[handle] * hit)
			if distance < closest:
				closest = distance
				result = handle
	return result
