extends SceneTree
## Independent decoder fixtures; these do not claim a live-server browser gate.

const Decoder = preload("res://protocol.gd")
var data = PackedByteArray()
var checks = 0
var failures = 0


func _initialize() -> void:
	var decoder = Decoder.new()
	data = PackedByteArray()
	u8(1)
	i16(2)
	i16(-1)
	u32(100)
	u16(1)
	entity(7, 19, 2, Vector3i(100, 200, 300), {117: 0.25, 209: 4.0})
	check(decoder.decode(data), decoder.error)
	check(decoder.entities.has("o:19.2"), "handle")
	check(
		decoder.entities["o:19.2"].position.is_equal_approx(Vector3(65, -30, 3)), "cell centimeters"
	)
	check(is_equal_approx(decoder.entities["o:19.2"].states[117], 0.25), "arbitrary state")
	data = PackedByteArray()
	u8(2)
	i16(2)
	i16(-1)
	u32(102)
	u16(0)
	u16(0)
	u16(1)
	u16(7)
	u8(1 | 8 | 16)
	variable(-3)
	variable(250)
	variable(-1)
	u8(1)
	u16(209)
	f32(8.5)
	u8(9)
	check(decoder.decode(data), decoder.error)
	check(
		decoder.entities["o:19.2"].position.is_equal_approx(Vector3(64.97, -27.5, 2.99)),
		"delta baseline"
	)
	check(not decoder.entities["o:19.2"].states.has(117), "states replace full map")
	check(decoder.entities["o:19.2"].flags == 9, "flags")
	# A removed local ID is immediately reused by an addition.
	data = PackedByteArray()
	u8(2)
	i16(2)
	i16(-1)
	u32(104)
	u16(1)
	u16(7)
	u16(1)
	entity(7, 20, 1, Vector3i(0, 0, 0), {})
	u16(0)
	check(decoder.decode(data), decoder.error)
	decoder.finish_batch()
	check(not decoder.entities.has("o:19.2") and decoder.entities.has("o:20.1"), "local reuse")
	# Same handle enters another stream before the old stream drops.
	data = PackedByteArray()
	u8(1)
	i16(32767)
	i16(32767)
	u32(106)
	u16(1)
	entity(1, 20, 1, Vector3i(6400, -3200, 0), {})
	check(decoder.decode(data), decoder.error)
	data = PackedByteArray()
	u8(3)
	i16(2)
	i16(-1)
	check(decoder.decode(data), decoder.error)
	decoder.finish_batch()
	check(decoder.entities.has("o:20.1"), "migration survives old drop")
	check(
		decoder.entities["o:20.1"].position.is_equal_approx(Vector3(64, -32, 0)), "global absolute"
	)
	var identity = Decoder.unpack_rotation(3 | (512 << 2) | (512 << 12) | (512 << 22))
	check(absf(identity.dot(Quaternion.IDENTITY)) > 0.99999, "smallest three identity")
	var rotated = Quaternion(Vector3.UP, 0.7)
	var converted = Decoder.rotation_to_view(rotated)
	check(converted.is_normalized(), "quaternion conversion")
	check(
		Decoder.to_server(Decoder.to_view(Vector3(1, 2, 3))) == Vector3(1, 2, 3), "axis roundtrip"
	)
	check(not decoder.decode(PackedByteArray([1, 0])), "truncated frame rejected")
	# Synthetic time samples verify the renderer's delayed interpolation independently
	# from the decoded authoritative baseline.
	var samples = []
	for step in [10, 20, 30, 40]:
		samples.append(
			{
				"step": step,
				"position": Vector3(step, 0, 0),
				"rotation": Quaternion(Vector3.UP, step * 0.01)
			}
		)
	decoder.entities["fixture"] = {
		"position": Vector3(40, 0, 0), "rotation": samples[-1].rotation, "samples": samples
	}
	decoder.last_receive_ms = 1000
	decoder.newest_step = 28
	check(
		decoder.sample("fixture", 1000).origin.is_equal_approx(Vector3(25, 0, 0)),
		"interpolation chooses the containing interval"
	)
	check(
		(
			absf(
				decoder.sample("fixture", 1000).basis.get_rotation_quaternion().dot(
					Decoder.rotation_to_view(Quaternion(Vector3.UP, 0.25))
				)
			)
			> 0.99999
		),
		"rotation interpolates the same interval"
	)
	decoder.configure({"cell_m": 64, "step_hz": 20, "repl_delay_ms": 250})
	check(
		decoder.cell_m == 64 and decoder.repl_delay_ms == 250,
		"welcome applies configured stream values"
	)
	check(
		decoder.sample("fixture", 1000).origin.is_equal_approx(Vector3(23, 0, 0)),
		"configured250ms delay chooses the actual earlier interval"
	)
	check(
		(
			absf(
				decoder.sample("fixture", 1000).basis.get_rotation_quaternion().dot(
					Decoder.rotation_to_view(Quaternion(Vector3.UP, 0.23))
				)
			)
			> 0.99999
		),
		"configured delay also controls rotation interpolation"
	)
	decoder.configure({})
	check(
		decoder.repl_delay_ms == 150, "synthetic welcome without configuration uses fixture default"
	)
	decoder.newest_step = 100
	check(decoder.sample("fixture", 1000).origin == Vector3(40, 0, 0), "newest sample clamps")
	decoder.newest_step = 1
	check(decoder.sample("fixture", 1000).origin == Vector3(10, 0, 0), "oldest sample clamps")
	decoder.newest_step = 28
	check(
		(
			decoder.sample("fixture", 1000, false).basis == Basis.IDENTITY
			and decoder.sample("fixture", 1000, false).origin.is_equal_approx(Vector3(25, 0, 0))
		),
		"face-motion sampling skips unused rotation without changing position"
	)
	print("S6 decoder fixtures: %d checks passed" % checks)
	quit(0 if failures == 0 else 1)


func check(condition: bool, message: String) -> void:
	if not condition:
		failures += 1
		push_error(message)
		return
	checks += 1


func u8(value: int):
	data.append(value & 255)


func u16(value: int):
	var at = data.size()
	data.resize(at + 2)
	data.encode_u16(at, value)


func i16(value: int):
	var at = data.size()
	data.resize(at + 2)
	data.encode_s16(at, value)


func u32(value: int):
	var at = data.size()
	data.resize(at + 4)
	data.encode_u32(at, value)


func i32(value: int):
	var at = data.size()
	data.resize(at + 4)
	data.encode_s32(at, value)


func u64(value: int):
	var at = data.size()
	data.resize(at + 8)
	data.encode_u64(at, value)


func f32(value: float):
	var at = data.size()
	data.resize(at + 4)
	data.encode_float(at, value)


func variable(value: int):
	var encoded = (value << 1) ^ (value >> 31)
	while encoded > 127:
		u8((encoded & 127) | 128)
		encoded >>= 7
	u8(encoded)


func entity(local: int, slot: int, generation: int, cm: Vector3i, states: Dictionary):
	u16(local)
	u64((generation << 32) | slot)
	for index in 32:
		u8(index)
	u64(123)
	u8(0)
	i32(cm.x)
	i32(cm.y)
	i32(cm.z)
	u32(3 | (512 << 2) | (512 << 12) | (512 << 22))
	u8(states.size())
	for term in states:
		u16(term)
		f32(states[term])
