extends RefCounted

# A small Standard MIDI File reader (formats 0 and 1). It keeps note on/off
# messages from every track, merged and converted to seconds with the file's
# tempo map. Everything else (program changes, controllers, SysEx, text) is
# skipped: the demo plays the whole score on whichever layers are enabled.

# Each event is [seconds, status, data1, data2]; note-offs are status 0x8n.
var events: Array = []
var length_seconds := 0.0


func load_file(path: String) -> bool:
	var bytes := FileAccess.get_file_as_bytes(path)
	if bytes.size() < 14 or bytes.slice(0, 4).get_string_from_ascii() != "MThd":
		push_error("Not a MIDI file: " + path)
		return false
	var track_count := _u16(bytes, 10)
	var division := _u16(bytes, 12)
	if division & 0x8000:
		push_error("SMPTE time division is not supported: " + path)
		return false

	var ticked := []   # [tick, order, status, data1, data2]
	var tempos := []   # [tick, microseconds per quarter note]
	var pos := 8 + _u32(bytes, 4)
	var order := 0
	for _track in track_count:
		if pos + 8 > bytes.size() or bytes.slice(pos, pos + 4).get_string_from_ascii() != "MTrk":
			break
		var end := pos + 8 + _u32(bytes, pos + 4)
		var i := pos + 8
		pos = end
		var tick := 0
		var running := 0
		while i < end:
			var delta := _vlq(bytes, i)
			tick += delta[0]
			i = delta[1]
			var status := bytes[i]
			if status == 0xFF:
				var meta := bytes[i + 1]
				var length := _vlq(bytes, i + 2)
				var data: int = length[1]
				if meta == 0x51 and length[0] == 3:
					tempos.append([tick, (bytes[data] << 16) | (bytes[data + 1] << 8) | bytes[data + 2]])
				i = data + length[0]
				continue
			if status == 0xF0 or status == 0xF7:
				var sysex := _vlq(bytes, i + 1)
				i = sysex[1] + sysex[0]
				continue
			if status & 0x80:
				running = status
				i += 1
			var kind := running & 0xF0
			var size := 1 if kind == 0xC0 or kind == 0xD0 else 2
			if kind == 0x90 or kind == 0x80:
				var velocity := bytes[i + 1]
				if kind == 0x90 and velocity == 0:
					kind = 0x80
				ticked.append([tick, order, kind | (running & 0x0F), bytes[i], velocity])
				order += 1
			i += size

	# Note-offs sort before note-ons on the same tick, so a repeated note
	# restarts instead of being cut by its own previous note-off.
	ticked.sort_custom(func(a, b):
		if a[0] != b[0]:
			return a[0] < b[0]
		var a_off: bool = a[2] & 0xF0 == 0x80
		var b_off: bool = b[2] & 0xF0 == 0x80
		if a_off != b_off:
			return a_off
		return a[1] < b[1])
	tempos.sort_custom(func(a, b): return a[0] < b[0])

	var tempo_index := 0
	var tempo := 500000.0
	var tempo_tick := 0
	var tempo_seconds := 0.0
	events.clear()
	for event in ticked:
		while tempo_index < tempos.size() and tempos[tempo_index][0] <= event[0]:
			tempo_seconds += (tempos[tempo_index][0] - tempo_tick) * tempo / (division * 1000000.0)
			tempo_tick = tempos[tempo_index][0]
			tempo = tempos[tempo_index][1]
			tempo_index += 1
		var seconds: float = tempo_seconds + (event[0] - tempo_tick) * tempo / (division * 1000000.0)
		events.append([seconds, event[2], event[3], event[4]])
	length_seconds = events[-1][0] if not events.is_empty() else 0.0
	return true


static func _u16(bytes: PackedByteArray, at: int) -> int:
	return (bytes[at] << 8) | bytes[at + 1]


static func _u32(bytes: PackedByteArray, at: int) -> int:
	return (bytes[at] << 24) | (bytes[at + 1] << 16) | (bytes[at + 2] << 8) | bytes[at + 3]


# Variable-length quantity: returns [value, index after it].
static func _vlq(bytes: PackedByteArray, at: int) -> Array:
	var value := 0
	while true:
		var b := bytes[at]
		at += 1
		value = (value << 7) | (b & 0x7F)
		if b < 0x80:
			break
	return [value, at]
