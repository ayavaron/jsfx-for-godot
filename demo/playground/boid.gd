extends Node2D

# A big triangle that flies between places the cursor has been. Clicking it
# emits `clicked`; the owner changes a sound parameter and calls punch() and
# show_label() so the change is seen as well as heard.

signal clicked

const SHAPE := [Vector2(34, 0), Vector2(-24, -22), Vector2(-24, 22)]
const FILL := Color(0.8, 0.8, 0.83)
const OUTLINE := Color(1, 1, 1)
const MAX_SPEED := 420.0
const STEER := 5.0           # how quickly velocity turns toward the target
const HISTORY := 32          # remembered cursor positions
const HISTORY_SPACING := 40.0

var velocity := Vector2(160, -60)
var _history: Array[Vector2] = []
var _target := Vector2.ZERO
var _retarget := 0.0
var _fill := FILL
var _punch := 1.0            # extra scale from a click, eased back to 1
var _hover := false
var _cursor := Vector2(-1000, -1000)


func _ready() -> void:
	_target = get_viewport_rect().size * 0.5
	position = _target + Vector2(-200, 80)


# --- Movement ----------------------------------------------------------------

func _input(event: InputEvent) -> void:
	if event is InputEventMouseMotion or event is InputEventScreenDrag:
		_cursor = event.position
		_remember(event.position)
		if event is InputEventMouseMotion:
			_set_hover(_hits(event.position))
	elif event is InputEventScreenTouch and event.pressed:
		_remember(event.position)
		if _hits(event.position):
			get_viewport().set_input_as_handled()
			clicked.emit()
	elif event is InputEventMouseButton and event.pressed \
			and event.button_index == MOUSE_BUTTON_LEFT \
			and event.device != InputEvent.DEVICE_ID_EMULATION:
		if _hits(event.position):
			get_viewport().set_input_as_handled()
			clicked.emit()


func _remember(point: Vector2) -> void:
	if _history.is_empty() or _history[-1].distance_to(point) > HISTORY_SPACING:
		_history.append(point)
		if _history.size() > HISTORY:
			_history.pop_front()


func _process(delta: float) -> void:
	_retarget -= delta
	if _retarget <= 0.0 or position.distance_to(_target) < 50.0:
		_pick_target()

	# Arrive: full speed far away, slowing in the last 160 px.
	var to_target := _target - position
	var speed := MAX_SPEED * clampf(to_target.length() / 160.0, 0.35, 1.0)
	# It dawdles near the cursor, so reaching for it feels fair.
	speed *= clampf(position.distance_to(_cursor) / 220.0, 0.15, 1.0)
	var desired := to_target.normalized() * speed
	velocity = velocity.lerp(desired, 1.0 - exp(-STEER * delta))
	position += velocity * delta
	if velocity.length() > 5.0:
		rotation = lerp_angle(rotation, velocity.angle(), 1.0 - exp(-10.0 * delta))
	queue_redraw()


func _pick_target() -> void:
	_retarget = randf_range(1.2, 2.6)
	var area := get_viewport_rect().size
	if _history.is_empty():
		_target = Vector2(randf_range(0.15, 0.85) * area.x, randf_range(0.2, 0.8) * area.y)
		return
	# Favor recent places, with a little scatter so it circles rather than
	# parking on one point.
	var index := _history.size() - 1 - int(pow(randf(), 2.0) * _history.size())
	_target = _history[clampi(index, 0, _history.size() - 1)] \
			+ Vector2(randf_range(-60, 60), randf_range(-60, 60))
	_target = _target.clamp(Vector2(40, 40), area - Vector2(40, 40))


# --- Hit testing and drawing ---------------------------------------------------

func _points(extra := 1.0) -> PackedVector2Array:
	var points := PackedVector2Array()
	for point in SHAPE:
		points.append(point * extra)
	return points


func _hits(screen_position: Vector2) -> bool:
	# Generous: an inflated triangle, in the boid's own space.
	var local := to_local(screen_position)
	return local.length() < 44.0 * _punch 			or Geometry2D.is_point_in_polygon(local, _points(1.5 * _punch))


func _set_hover(hover: bool) -> void:
	if hover == _hover:
		return
	_hover = hover
	Input.set_default_cursor_shape(Input.CURSOR_POINTING_HAND if hover else Input.CURSOR_ARROW)


func _draw() -> void:
	var points := _points(_punch)
	draw_colored_polygon(points, _fill)
	var outline := points.duplicate()
	outline.append(points[0])
	draw_polyline(outline, OUTLINE, (7.0 if _hover else 5.0) * sqrt(_punch), true)


# --- Juice ---------------------------------------------------------------------

# Grow to a random size in a random bright colour, then settle back in ~0.2 s.
func punch() -> Color:
	var color := Color.from_hsv(randf(), 1.0, 1.0)
	var size := randf_range(1.8, 5.3)
	_fill = color
	_punch = size
	velocity += Vector2.from_angle(randf() * TAU) * 520.0
	var tween := create_tween().set_parallel()
	tween.tween_property(self, "_punch", 1.0, 0.2) \
			.set_trans(Tween.TRANS_BACK).set_ease(Tween.EASE_OUT)
	tween.tween_property(self, "_fill", FILL, 0.2)
	_spawn_ring(color, size)
	return color


func _spawn_ring(color: Color, size: float) -> void:
	var ring := Ring.new()
	ring.position = position
	ring.color = color
	ring.radius = 30.0
	get_parent().add_child(ring)
	var tween := ring.create_tween().set_parallel()
	tween.tween_property(ring, "radius", 70.0 + size * 30.0, 0.35) \
			.set_trans(Tween.TRANS_QUAD).set_ease(Tween.EASE_OUT)
	tween.tween_property(ring, "modulate:a", 0.0, 0.35)
	tween.chain().tween_callback(ring.queue_free)


# A floating caption that rises from the boid and fades.
func show_label(text: String, color: Color) -> void:
	var label := Label.new()
	label.text = text
	label.add_theme_font_size_override("font_size", 24)
	label.add_theme_color_override("font_color", color)
	label.add_theme_color_override("font_outline_color", Color(0.05, 0.05, 0.08))
	label.add_theme_constant_override("outline_size", 8)
	get_parent().add_child(label)
	# Keep the caption on screen even when the boid is at an edge.
	var width := label.get_combined_minimum_size().x
	var area := get_viewport_rect().size
	label.position = Vector2(clampf(position.x - width * 0.5, 8.0, area.x - width - 8.0),
			clampf(position.y - 80.0, 80.0, area.y - 40.0))
	var tween := label.create_tween().set_parallel()
	tween.tween_property(label, "position:y", label.position.y - 70.0, 1.1) \
			.set_trans(Tween.TRANS_CUBIC).set_ease(Tween.EASE_OUT)
	tween.tween_property(label, "modulate:a", 0.0, 1.1).set_delay(0.4)
	tween.chain().tween_callback(label.queue_free)


class Ring extends Node2D:
	var color := Color.WHITE
	var radius := 30.0:
		set(value):
			radius = value
			queue_redraw()

	func _draw() -> void:
		draw_arc(Vector2.ZERO, radius, 0.0, TAU, 48, color, 6.0, true)
