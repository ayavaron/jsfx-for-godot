extends Control

# The playable demo: a JSFX synth (AudioStreamJSFX) on its own bus with two JSFX
# bus effects. Every knob is generated from the effect's slider properties, so
# this page is also a picture of what AudioEffectJSFX exposes to GDScript.
#
# Play with the on-screen keys (mouse or touch) or the computer keyboard:
# A W S E D F T G Y H U J K O L P ; with Z / X to shift the octave.

const Piano := preload("res://playground/piano.gd")

const SYNTH_PATH := "res://instruments/simple_synth.jsfx"
const EFFECTS := [
	["Tremolo", "res://effects/tremolo.jsfx"],
	["Resonant lowpass", "res://effects/lowpass.jsfx"],
]
const BUS_NAME := "Synth"

# Physical keys -> semitones above the keyboard octave's C.
const KEYMAP := {
	KEY_A: 0, KEY_W: 1, KEY_S: 2, KEY_E: 3, KEY_D: 4, KEY_F: 5, KEY_T: 6,
	KEY_G: 7, KEY_Y: 8, KEY_H: 9, KEY_U: 10, KEY_J: 11, KEY_K: 12, KEY_O: 13,
	KEY_L: 14, KEY_P: 15, KEY_SEMICOLON: 16,
}

# Am - F - C - G, played as a rising and falling arpeggio, 8 steps per chord.
const PROGRESSION := [[57, 60, 64, 69], [53, 57, 60, 65], [48, 55, 60, 64], [55, 59, 62, 67]]
const ARP_PATTERN := [0, 1, 2, 3, 2, 1, 2, 3]
const ARP_STEP_SECONDS := 0.14

const PANEL_COLOR := Color(0.12, 0.13, 0.18)
const ACCENT := Color(0.98, 0.65, 0.25)
const MUTED := Color(0.62, 0.63, 0.68)

var _synth: AudioEffectJSFX
var _bus := -1
var _piano: Piano
var _held := {}           # note -> number of sources holding it
var _key_notes := {}      # physical key -> note it started
var _octave := 0          # keyboard octave offset from C3
var _octave_label: Label
var _bend: HSlider
var _meter: ProgressBar
var _arp_on := false
var _arp_timer := 0.0
var _arp_step := 0
var _arp_note := -1
var _overlay: Control
var _sound_reported := false


func _ready() -> void:
	# The browser smoke test reuses this export: ?smoke=1 (or its probe
	# parameters) switches to the test harness scene instead.
	if OS.has_feature("web"):
		var query := str(JavaScriptBridge.eval("location.search"))
		for parameter in ["smoke=", "instances=", "synth="]:
			if query.contains(parameter):
				get_tree().change_scene_to_file.call_deferred("res://web_smoke.tscn")
				return

	_setup_audio()
	_build_ui()
	if OS.has_feature("web"):
		_build_start_overlay()
	print("JSFX_PLAYGROUND_READY")


func _setup_audio() -> void:
	_bus = AudioServer.bus_count
	AudioServer.add_bus(_bus)
	AudioServer.set_bus_name(_bus, BUS_NAME)

	var stream := AudioStreamJSFX.new()
	stream.jsfx = load(SYNTH_PATH)
	_synth = stream.jsfx
	_synth.set("slider_4_release_ms", 500.0)

	var player := AudioStreamPlayer.new()
	player.stream = stream
	player.bus = BUS_NAME
	# Web defaults to sample playback, which skips Godot's mixer; JSFX streams
	# always mix through it.
	player.playback_type = AudioServer.PLAYBACK_TYPE_STREAM
	add_child(player)
	# On Web this starts before the first click, and is heard once the browser
	# lets Godot resume its AudioContext.
	player.play()

	for i in EFFECTS.size():
		var effect := AudioEffectJSFX.new()
		effect.script_path = EFFECTS[i][1]
		AudioServer.add_bus_effect(_bus, effect)
		AudioServer.set_bus_effect_enabled(_bus, i, false)


# --- UI ----------------------------------------------------------------------

func _build_ui() -> void:
	set_anchors_preset(PRESET_FULL_RECT)

	var margin := MarginContainer.new()
	margin.set_anchors_preset(PRESET_FULL_RECT)
	for side in ["left", "right", "top", "bottom"]:
		margin.add_theme_constant_override("margin_" + side, 24)
	add_child(margin)

	var column := VBoxContainer.new()
	column.add_theme_constant_override("separation", 16)
	margin.add_child(column)

	var title := Label.new()
	title.text = "JSFX for Godot"
	title.add_theme_font_size_override("font_size", 30)
	column.add_child(title)
	var subtitle := Label.new()
	subtitle.text = ("A REAPER JSFX synth playing inside Godot's AudioServer. "
			+ "Every knob below is a JSFX slider exposed as a Godot property.")
	subtitle.add_theme_color_override("font_color", MUTED)
	subtitle.autowrap_mode = TextServer.AUTOWRAP_WORD_SMART
	column.add_child(subtitle)

	var rack := HBoxContainer.new()
	rack.add_theme_constant_override("separation", 16)
	rack.size_flags_vertical = SIZE_SHRINK_BEGIN
	column.add_child(rack)

	var synth_rows := _panel(rack, "Synth  ·  simple_synth.jsfx")
	_add_slider_controls(synth_rows, _synth)
	for i in EFFECTS.size():
		var effect := AudioServer.get_bus_effect(_bus, i) as AudioEffectJSFX
		var rows := _panel(rack, EFFECTS[i][0], i)
		_add_slider_controls(rows, effect)
	_build_performance_panel(rack)

	_piano = Piano.new()
	_piano.first_note = 48
	_piano.key_count = 25
	_piano.custom_minimum_size = Vector2(0, 190)
	_piano.size_flags_vertical = SIZE_EXPAND_FILL
	_piano.note_pressed.connect(_press)
	_piano.note_released.connect(_release)
	column.add_child(_piano)

	var help := Label.new()
	help.text = ("Play: click or touch the keys, or type A W S E D F T G Y H U J K O L P ;"
			+ "   ·   Z / X: octave down / up")
	help.add_theme_color_override("font_color", MUTED)
	column.add_child(help)


# A titled panel; returns the container rows go in. With an effect index, the
# title gets an on/off switch that enables that bus effect.
func _panel(parent: Control, title_text: String, effect_index := -1) -> VBoxContainer:
	var panel := PanelContainer.new()
	var style := StyleBoxFlat.new()
	style.bg_color = PANEL_COLOR
	style.set_corner_radius_all(8)
	style.set_content_margin_all(14)
	panel.add_theme_stylebox_override("panel", style)
	panel.size_flags_horizontal = SIZE_EXPAND_FILL
	parent.add_child(panel)

	var rows := VBoxContainer.new()
	rows.add_theme_constant_override("separation", 6)
	panel.add_child(rows)

	var header := HBoxContainer.new()
	rows.add_child(header)
	var title := Label.new()
	title.text = title_text
	title.add_theme_color_override("font_color", ACCENT)
	title.size_flags_horizontal = SIZE_EXPAND_FILL
	header.add_child(title)
	if effect_index >= 0:
		var toggle := CheckButton.new()
		toggle.focus_mode = FOCUS_NONE
		toggle.toggled.connect(func(on: bool) -> void:
			AudioServer.set_bus_effect_enabled(_bus, effect_index, on))
		header.add_child(toggle)
	return rows


# One control per JSFX slider, read from the effect's property list: range
# sliders become HSliders (log-shaped ones exponential), enum sliders become
# option buttons. Changing a control sets the property, as gameplay code would.
func _add_slider_controls(rows: VBoxContainer, effect: AudioEffectJSFX) -> void:
	for property in effect.get_property_list():
		var name: String = property.name
		if not name.begins_with("slider_"):
			continue
		var row := HBoxContainer.new()
		rows.add_child(row)
		var label := Label.new()
		label.text = _pretty_name(name)
		label.custom_minimum_size = Vector2(112, 0)
		row.add_child(label)

		if property.hint == PROPERTY_HINT_ENUM:
			var options := OptionButton.new()
			options.focus_mode = FOCUS_NONE
			options.size_flags_horizontal = SIZE_EXPAND_FILL
			for item in String(property.hint_string).split(","):
				options.add_item(item.get_slice(":", 0), int(item.get_slice(":", 1)))
			options.select(options.get_item_index(int(effect.get(name))))
			options.item_selected.connect(func(index: int) -> void:
				effect.set(name, options.get_item_id(index)))
			row.add_child(options)
			continue

		var hint := String(property.hint_string).split(",")
		var slider := HSlider.new()
		slider.focus_mode = FOCUS_NONE
		slider.size_flags_horizontal = SIZE_EXPAND_FILL
		slider.size_flags_vertical = SIZE_SHRINK_CENTER
		slider.min_value = float(hint[0])
		slider.max_value = float(hint[1])
		slider.step = float(hint[2]) if hint.size() > 2 else 0.0
		slider.exp_edit = hint.has("exp")
		slider.value = float(effect.get(name))
		row.add_child(slider)

		var value_label := Label.new()
		value_label.custom_minimum_size = Vector2(56, 0)
		value_label.horizontal_alignment = HORIZONTAL_ALIGNMENT_RIGHT
		value_label.add_theme_color_override("font_color", MUTED)
		value_label.text = _format_value(slider.value)
		row.add_child(value_label)

		slider.value_changed.connect(func(value: float) -> void:
			effect.set(name, value)
			value_label.text = _format_value(value))


func _build_performance_panel(rack: Control) -> void:
	var rows := _panel(rack, "Play")

	var mod := _labeled_slider(rows, "Mod wheel", 0.0, 127.0, 1.0)
	mod.value_changed.connect(func(value: float) -> void:
		_synth.control_change(1, int(value)))

	# Pitch bend springs back to the centre when let go, like a real wheel.
	_bend = _labeled_slider(rows, "Pitch bend", -1.0, 1.0, 0.01)
	_bend.value_changed.connect(func(value: float) -> void:
		_synth.pitch_bend(value))
	_bend.drag_ended.connect(func(_changed: bool) -> void:
		_bend.value = 0.0)

	var octave_row := HBoxContainer.new()
	rows.add_child(octave_row)
	var octave_title := Label.new()
	octave_title.text = "Octave"
	octave_title.custom_minimum_size = Vector2(112, 0)
	octave_row.add_child(octave_title)
	var down := Button.new()
	down.text = "  -  "
	down.focus_mode = FOCUS_NONE
	down.pressed.connect(_shift_octave.bind(-1))
	octave_row.add_child(down)
	_octave_label = Label.new()
	_octave_label.custom_minimum_size = Vector2(48, 0)
	_octave_label.horizontal_alignment = HORIZONTAL_ALIGNMENT_CENTER
	octave_row.add_child(_octave_label)
	var up := Button.new()
	up.text = "  +  "
	up.focus_mode = FOCUS_NONE
	up.pressed.connect(_shift_octave.bind(1))
	octave_row.add_child(up)
	_shift_octave(0)

	var arp := CheckButton.new()
	arp.text = "Arpeggiator"
	arp.focus_mode = FOCUS_NONE
	arp.toggled.connect(_set_arpeggiator)
	rows.add_child(arp)

	var meter_row := HBoxContainer.new()
	rows.add_child(meter_row)
	var meter_title := Label.new()
	meter_title.text = "Level"
	meter_title.custom_minimum_size = Vector2(112, 0)
	meter_row.add_child(meter_title)
	_meter = ProgressBar.new()
	_meter.min_value = -60.0
	_meter.max_value = 0.0
	_meter.show_percentage = false
	_meter.size_flags_horizontal = SIZE_EXPAND_FILL
	_meter.size_flags_vertical = SIZE_SHRINK_CENTER
	_meter.custom_minimum_size = Vector2(0, 10)
	var fill := StyleBoxFlat.new()
	fill.bg_color = ACCENT
	_meter.add_theme_stylebox_override("fill", fill)
	meter_row.add_child(_meter)


func _labeled_slider(rows: VBoxContainer, text: String, min_value: float,
		max_value: float, step: float) -> HSlider:
	var row := HBoxContainer.new()
	rows.add_child(row)
	var label := Label.new()
	label.text = text
	label.custom_minimum_size = Vector2(112, 0)
	row.add_child(label)
	var slider := HSlider.new()
	slider.focus_mode = FOCUS_NONE
	slider.min_value = min_value
	slider.max_value = max_value
	slider.step = step
	slider.size_flags_horizontal = SIZE_EXPAND_FILL
	slider.size_flags_vertical = SIZE_SHRINK_CENTER
	row.add_child(slider)
	return slider


# Browsers start Web Audio suspended until the page gets a click or key press.
func _build_start_overlay() -> void:
	_overlay = ColorRect.new()
	_overlay.color = Color(0.0, 0.0, 0.0, 0.7)
	_overlay.set_anchors_preset(PRESET_FULL_RECT)
	_overlay.gui_input.connect(func(event: InputEvent) -> void:
		if event.is_pressed():
			_dismiss_overlay())
	add_child(_overlay)
	var start := Button.new()
	start.text = "   Click to start audio   "
	start.add_theme_font_size_override("font_size", 26)
	start.set_anchors_preset(PRESET_CENTER)
	start.grow_horizontal = GROW_DIRECTION_BOTH
	start.grow_vertical = GROW_DIRECTION_BOTH
	start.pressed.connect(_dismiss_overlay)
	_overlay.add_child(start)


func _dismiss_overlay() -> void:
	if _overlay != null:
		_overlay.queue_free()
		_overlay = null


static func _pretty_name(property_name: String) -> String:
	# slider_5_cutoff_hz -> "Cutoff (Hz)"
	var words := property_name.substr(property_name.find("_", 7) + 1).split("_")
	var unit := ""
	if words.size() > 1 and words[-1] in ["ms", "hz", "db"]:
		unit = {"ms": " (ms)", "hz": " (Hz)", "db": " (dB)"}[words[-1]]
		words.remove_at(words.size() - 1)
	return " ".join(words).capitalize() + unit


static func _format_value(value: float) -> String:
	if absf(value) >= 100.0:
		return str(roundi(value))
	return "%.2f" % value


# --- Notes -------------------------------------------------------------------

# Every note source (on-screen keys, computer keys, arpeggiator) goes through
# here, so a note held twice only sounds once and stops when both let go.
func _press(note: int) -> void:
	_held[note] = _held.get(note, 0) + 1
	if _held[note] == 1:
		_synth.note_on(note, 100)
		_piano.set_lit(note, true)


func _release(note: int) -> void:
	if not _held.has(note):
		return
	_held[note] -= 1
	if _held[note] <= 0:
		_held.erase(note)
		_synth.note_off(note)
		_piano.set_lit(note, false)


func _unhandled_input(event: InputEvent) -> void:
	if _overlay != null and event.is_pressed() and not event.is_echo():
		_dismiss_overlay()
	if not event is InputEventKey or event.echo:
		return
	var key: int = event.physical_keycode
	if event.pressed and key == KEY_Z:
		_shift_octave(-1)
	elif event.pressed and key == KEY_X:
		_shift_octave(1)
	elif KEYMAP.has(key):
		if event.pressed and not _key_notes.has(key):
			_key_notes[key] = 48 + 12 * _octave + KEYMAP[key]
			_press(_key_notes[key])
		elif not event.pressed and _key_notes.has(key):
			_release(_key_notes[key])
			_key_notes.erase(key)
	else:
		return
	get_viewport().set_input_as_handled()


func _shift_octave(delta: int) -> void:
	_octave = clampi(_octave + delta, -2, 3)
	_octave_label.text = "C%d" % (3 + _octave)


func _set_arpeggiator(on: bool) -> void:
	_arp_on = on
	_arp_timer = 0.0
	_arp_step = 0
	if not on and _arp_note >= 0:
		_release(_arp_note)
		_arp_note = -1


func _notification(what: int) -> void:
	# Keys released while the window is unfocused never send key-up events.
	if what == NOTIFICATION_APPLICATION_FOCUS_OUT and _synth != null:
		for key in _key_notes:
			_release(_key_notes[key])
		_key_notes.clear()
		_piano.release_all()


func _process(delta: float) -> void:
	if _synth == null:
		return
	if _arp_on:
		_arp_timer -= delta
		if _arp_timer <= 0.0:
			_arp_timer += ARP_STEP_SECONDS
			var chord: Array = PROGRESSION[(_arp_step / ARP_PATTERN.size()) % PROGRESSION.size()]
			if _arp_note >= 0:
				_release(_arp_note)
			_arp_note = chord[ARP_PATTERN[_arp_step % ARP_PATTERN.size()]]
			_press(_arp_note)
			_arp_step += 1

	var peak := maxf(AudioServer.get_bus_peak_volume_left_db(_bus, 0),
			AudioServer.get_bus_peak_volume_right_db(_bus, 0))
	_meter.value = maxf(peak, -60.0)
	# Marker for tools/browser_smoke.py --playground.
	if not _sound_reported and peak > -40.0:
		_sound_reported = true
		print("JSFX_PLAYGROUND_SOUND peak_db=", peak)
