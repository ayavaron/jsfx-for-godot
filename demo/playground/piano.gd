extends Control

# An on-screen piano for mouse and touch. Each finger (or the mouse) holds one
# note; dragging across keys plays a glissando. Keys light up for notes held by
# any source, which the owner reports with set_lit().

signal note_pressed(note: int)
signal note_released(note: int)

const BLACK_STEPS := [1, 3, 6, 8, 10]
const MOUSE_POINTER := -1

const WHITE := Color(0.95, 0.93, 0.89)
const WHITE_LIT := Color(0.98, 0.72, 0.36)
const BLACK := Color(0.11, 0.12, 0.15)
const BLACK_LIT := Color(0.93, 0.55, 0.18)
const EDGE := Color(0.07, 0.08, 0.11)
const LABEL := Color(0.45, 0.45, 0.48)

var first_note := 48
var key_count := 25

var _lit := {}       # note -> true
var _pointers := {}  # pointer index -> note it holds (-1 when off the keys)


func _ready() -> void:
	mouse_filter = MOUSE_FILTER_STOP
	resized.connect(queue_redraw)


func set_lit(note: int, lit: bool) -> void:
	if lit:
		_lit[note] = true
	else:
		_lit.erase(note)
	queue_redraw()


func release_all() -> void:
	for index in _pointers.keys():
		_move_pointer(index, -1)
	_pointers.clear()


static func is_black(note: int) -> bool:
	return BLACK_STEPS.has(note % 12)


func _white_count() -> int:
	var count := 0
	for note in range(first_note, first_note + key_count):
		if not is_black(note):
			count += 1
	return count


# Rectangles for every key, blacks first so hit tests find them on top.
func _key_rects() -> Array:
	var whites := []
	var blacks := []
	var white_width: float = size.x / maxi(_white_count(), 1)
	var white_index := 0
	for note in range(first_note, first_note + key_count):
		if is_black(note):
			var center: float = white_index * white_width
			blacks.append([note, Rect2(center - white_width * 0.3, 0.0,
					white_width * 0.6, size.y * 0.62)])
		else:
			whites.append([note, Rect2(white_index * white_width, 0.0,
					white_width, size.y)])
			white_index += 1
	return blacks + whites


func note_at(position: Vector2) -> int:
	for key in _key_rects():
		if key[1].has_point(position):
			return key[0]
	return -1


func _draw() -> void:
	var rects := _key_rects()
	var font := ThemeDB.fallback_font
	# Whites are at the end of the list; draw them first.
	for i in range(rects.size() - 1, -1, -1):
		var note: int = rects[i][0]
		var rect: Rect2 = rects[i][1]
		if is_black(note):
			continue
		draw_rect(rect, WHITE_LIT if _lit.has(note) else WHITE)
		draw_rect(rect, EDGE, false, 2.0)
		if note % 12 == 0:
			draw_string(font, Vector2(rect.position.x, rect.end.y - 12.0),
					"C%d" % (floori(note / 12.0) - 1), HORIZONTAL_ALIGNMENT_CENTER,
					rect.size.x, 14, LABEL)
	for key in rects:
		if is_black(key[0]):
			draw_rect(key[1], BLACK_LIT if _lit.has(key[0]) else BLACK)


func _gui_input(event: InputEvent) -> void:
	# Touches arrive as screen events; the mouse copies Godot synthesizes from
	# them (device -1) are skipped so a tap plays once.
	if event is InputEventScreenTouch:
		if event.pressed:
			_pointers[event.index] = -1
			_move_pointer(event.index, note_at(event.position))
		elif _pointers.has(event.index):
			_move_pointer(event.index, -1)
			_pointers.erase(event.index)
		accept_event()
	elif event is InputEventScreenDrag:
		if _pointers.has(event.index):
			_move_pointer(event.index, note_at(event.position))
		accept_event()
	elif event is InputEventMouseButton and event.device != InputEvent.DEVICE_ID_EMULATION:
		if event.button_index != MOUSE_BUTTON_LEFT:
			return
		if event.pressed:
			_pointers[MOUSE_POINTER] = -1
			_move_pointer(MOUSE_POINTER, note_at(event.position))
		elif _pointers.has(MOUSE_POINTER):
			_move_pointer(MOUSE_POINTER, -1)
			_pointers.erase(MOUSE_POINTER)
		accept_event()
	elif event is InputEventMouseMotion and event.device != InputEvent.DEVICE_ID_EMULATION:
		if _pointers.has(MOUSE_POINTER):
			_move_pointer(MOUSE_POINTER, note_at(event.position))
			accept_event()


func _move_pointer(index: int, note: int) -> void:
	var held: int = _pointers.get(index, -1)
	if held == note:
		return
	if held >= 0:
		note_released.emit(held)
	_pointers[index] = note
	if note >= 0:
		note_pressed.emit(note)
