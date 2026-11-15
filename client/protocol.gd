extends RefCounted
## Exact S6 stream state. Render interpolation never mutates these wire baselines.

signal entity_changed(handle: String)
signal entity_removed(handle: String)

const GLOBAL_CELL = 32767
const QUAT_LIMIT = 0.7071067811865476
var cell_m = 32.0
var step_hz = 20.0
var repl_delay_ms = 150.0
var entities: Dictionary = {}
var cells: Dictionary = {}
var newest_step = 0
var last_receive_ms = 0
var bytes_received = 0
var error = ""
var _bytes = PackedByteArray()
var _offset = 0
var _valid = true


func configure(welcome: Dictionary) -> void:
	cell_m = float(welcome.get("cell_m", 32))
	step_hz = float(welcome.get("step_hz", 20))
	repl_delay_ms = float(welcome.get("repl_delay_ms", 150))


func reset() -> void:
	for handle in entities:
		entity_removed.emit(handle)
	entities.clear()
	cells.clear()
	newest_step = 0
	last_receive_ms = 0


func _need(count: int) -> bool:
	if _offset + count > _bytes.size():
		_valid = false
		error = "Truncated replication frame"
	return _valid


func _u8() -> int:
	if not _need(1):
		return 0
	var result = _bytes[_offset]
	_offset += 1
	return result


func _u16() -> int:
	if not _need(2):
		return 0
	var result = _bytes.decode_u16(_offset)
	_offset += 2
	return result


func _i16() -> int:
	if not _need(2):
		return 0
	var result = _bytes.decode_s16(_offset)
	_offset += 2
	return result


func _u32() -> int:
	if not _need(4):
		return 0
	var result = _bytes.decode_u32(_offset)
	_offset += 4
	return result


func _i32() -> int:
	if not _need(4):
		return 0
	var result = _bytes.decode_s32(_offset)
	_offset += 4
	return result


func _u64() -> int:
	if not _need(8):
		return 0
	var result = _bytes.decode_u64(_offset)
	_offset += 8
	return result


func _f32() -> float:
	if not _need(4):
		return 0
	var result = _bytes.decode_float(_offset)
	_offset += 4
	return result


func _hash() -> String:
	if not _need(32):
		return ""
	var result = _bytes.slice(_offset, _offset + 32).hex_encode()
	_offset += 32
	return result


func _signed_varint() -> int:
	var value = 0
	for shift in range(0, 35, 7):
		var byte = _u8()
		value |= (byte & 127) << shift
		if (byte & 128) == 0:
			return (value >> 1) ^ -(value & 1)
	_valid = false
	error = "Invalid position varint"
	return 0


static func unpack_rotation(packed: int) -> Quaternion:
	var largest = packed & 3
	var values = [0.0, 0.0, 0.0, 0.0]
	var component = 0
	var squared = 0.0
	for axis in 4:
		if axis == largest:
			continue
		var encoded = (packed >> (2 + component * 10)) & 1023
		values[axis] = (float(encoded) / 1023.0 * 2.0 - 1.0) * QUAT_LIMIT
		squared += values[axis] * values[axis]
		component += 1
	values[largest] = sqrt(maxf(0.0, 1.0 - squared))
	return Quaternion(values[0], values[1], values[2], values[3]).normalized()


static func to_view(vector: Vector3) -> Vector3:
	return Vector3(vector.x, vector.z, -vector.y)


static func to_server(vector: Vector3) -> Vector3:
	return Vector3(vector.x, -vector.z, vector.y)


static func rotation_to_view(rotation: Quaternion) -> Quaternion:
	var axis = Quaternion(Vector3.RIGHT, -PI / 2.0)
	return axis * rotation * axis.inverse()


func _position(cell: Vector2i, centimeters: Vector3i) -> Vector3:
	var result = Vector3(centimeters) * 0.01
	if cell != Vector2i(GLOBAL_CELL, GLOBAL_CELL):
		result += Vector3(cell.x * cell_m, cell.y * cell_m, 0.0)
	return result


func _states() -> Dictionary:
	var values = {}
	var count = _u8()
	for index in count:
		var term = _u16()
		values[term] = _f32()
	return values


func _entity(cell: Vector2i) -> Dictionary:
	var local = _u16()
	var bits = _u64()
	var handle = "o:%d.%d" % [bits & 0xffffffff, (bits >> 32) & 0xffffffff]
	var row = {"local": local, "h": handle, "asset": _hash(), "params": _u64(), "flags": _u8()}
	row.cm = Vector3i(_i32(), _i32(), _i32())
	row.rotation = unpack_rotation(_u32())
	row.states = _states()
	row.position = _position(cell, row.cm)
	return row


func _put(cell: Vector2i, row: Dictionary, step: int) -> void:
	if not _valid:
		return
	var handle = row.h
	cells[cell][row.local] = row
	if not entities.has(handle):
		entities[handle] = {"h": handle, "samples": [], "cells": {}}
	var entity = entities[handle]
	entity.receive_ms = Time.get_ticks_msec()
	entity.cells[cell] = true
	for key in ["asset", "params", "flags", "states", "position", "rotation"]:
		entity[key] = row[key]
	var samples = entity.samples
	if samples.is_empty() or samples[-1].step < step:
		samples.append({"step": step, "position": row.position, "rotation": row.rotation})
		if samples.size() > 5:
			samples.pop_front()
	elif samples[-1].step == step:
		samples[-1] = {"step": step, "position": row.position, "rotation": row.rotation}
	entity_changed.emit(handle)


func _remove(cell: Vector2i, local: int) -> void:
	if not cells.has(cell) or not cells[cell].has(local):
		return
	var handle = cells[cell][local].h
	cells[cell].erase(local)
	if entities.has(handle):
		entities[handle].cells.erase(cell)


func decode(bytes: PackedByteArray) -> bool:
	_bytes = bytes
	_offset = 0
	_valid = true
	error = ""
	bytes_received += bytes.size()
	var kind = _u8()
	var cell = Vector2i(_i16(), _i16())
	if not _valid:
		return false
	if kind == 3:
		if cells.has(cell):
			for local in cells[cell].keys():
				_remove(cell, local)
			cells.erase(cell)
	elif kind == 1 or kind == 2:
		var step = _u32()
		newest_step = maxi(newest_step, step)
		last_receive_ms = Time.get_ticks_msec()
		if kind == 1:
			if cells.has(cell):
				for local in cells[cell].keys():
					_remove(cell, local)
			cells[cell] = {}
			var count = _u16()
			for index in count:
				_put(cell, _entity(cell), step)
		else:
			if not cells.has(cell):
				error = "Delta received without keyframe"
				return false
			var removed = _u16()
			for index in removed:
				_remove(cell, _u16())
			var added = _u16()
			for index in added:
				_put(cell, _entity(cell), step)
			var changed = _u16()
			for index in changed:
				var local = _u16()
				var mask = _u8()
				if not cells[cell].has(local):
					error = "Delta references an unknown local entity"
					return false
				var row = cells[cell][local]
				if mask & 1:
					row.cm += Vector3i(_signed_varint(), _signed_varint(), _signed_varint())
					row.position = _position(cell, row.cm)
				if mask & 2:
					row.rotation = unpack_rotation(_u32())
				if mask & 4:
					row.asset = _hash()
					row.params = _u64()
				if mask & 8:
					row.states = _states()
				if mask & 16:
					row.flags = _u8()
				if mask & 224:
					error = "Unknown change mask"
					return false
				_put(cell, row, step)
	else:
		error = "Unknown binary frame type"
		return false
	if _offset != bytes.size():
		_valid = false
		error = "Unexpected replication frame length"
	return _valid


func finish_batch() -> void:
	for handle in entities.keys():
		if entities[handle].cells.is_empty():
			entities.erase(handle)
			entity_removed.emit(handle)


func sample(handle: String, now_ms: int, apply_rotation = true) -> Transform3D:
	var entity = entities[handle]
	var samples = entity.samples
	var target_step = (
		newest_step
		+ minf((now_ms - last_receive_ms) * step_hz / 1000.0, 10.0)
		- repl_delay_ms * step_hz / 1000.0
	)
	var position = entity.position
	var rotation = entity.rotation
	if not samples.is_empty():
		position = samples[0].position
		rotation = samples[0].rotation
		for index in range(samples.size() - 1, 0, -1):
			var a = samples[index - 1]
			var b = samples[index]
			if target_step >= a.step:
				if target_step >= b.step:
					position = b.position
					rotation = b.rotation
				else:
					var weight = (target_step - a.step) / maxf(1.0, b.step - a.step)
					position = a.position.lerp(b.position, weight)
					if apply_rotation:
						rotation = a.rotation.slerp(b.rotation, weight)
				break
	return Transform3D(
		Basis(rotation_to_view(rotation)) if apply_rotation else Basis.IDENTITY, to_view(position)
	)
