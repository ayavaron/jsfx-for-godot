extends Control

# The playable demo: three JSFX instruments (AudioStreamJSFX), each on its own
# bus, layered into a Mix bus that carries three JSFX effects. Every knob is
# generated from the script's slider properties, so this page is also a
# picture of what AudioEffectJSFX exposes to GDScript.
#
# Play with the on-screen keys (mouse or touch), the computer keyboard
# (A W S E D F T G Y H U J K O L P ; with Z / X to shift the octave), the
# arpeggiator or the demo track. Click the boid to change a parameter.

const Piano := preload("res://playground/piano.gd")
const Sequencer := preload("res://playground/sequencer.gd")
const Boid := preload("res://playground/boid.gd")

# Instrument layers. `percussion` marks the layer that also gets the demo
# track's drum channel (MIDI channel 10), which no other layer plays.
const LAYERS := [
	{"name": "Simple Synth", "path": "res://instruments/simple_synth.jsfx",
		"on": true, "db": -4.0, "percussion": false},
	{"name": "Unreal Piano", "path": "res://instruments/aya/gemini_unreal_piano.jsfx",
		"on": true, "db": 0.0, "percussion": false},
	{"name": "Cicada Swarm", "path": "res://instruments/aya/cicada_swarm.jsfx",
		"on": true, "db": -2.0, "percussion": true},
]
const EFFECTS := [
	{"name": "Tremolo", "path": "res://effects/tremolo.jsfx"},
	{"name": "Resonant lowpass", "path": "res://effects/lowpass.jsfx"},
	{"name": "Neighborhood", "path": "res://instruments/aya/gemini_neighborhood.jsfx"},
]
const MIX_BUS := "Mix"
const LOFI_FACTORS := [1, 2, 4, 8]
const DEMO_TRACK := "res://music/barber_of_seville_overture.mid"
const DRUM_CHANNEL := 9

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

# The boid leaves levels alone: a random master volume of zero is a dud.
const BOID_SKIP := ["volume", "gain", "master", "output", "dry_wet", "wet_dry"]

const PANEL_COLOR := Color(0.12, 0.13, 0.18)
const ACCENT := Color(0.98, 0.65, 0.25)
const MUTED := Color(0.62, 0.63, 0.68)
const LABEL_WIDTH := 150

# Runtime layer state: {effect, bus, player, enabled, held (note -> count)}.
var _layers: Array = []
var _mix_bus := -1
var _piano: Piano
var _sequencer: Sequencer
var _boid: Boid
var _lit := {}            # note -> number of sources lighting it
var _key_notes := {}      # physical key -> note it started
var _octave := 0          # keyboard octave offset from C3
var _octave_label: Label
var _bend: HSlider
var _meter: ProgressBar
var _fps: Label
var _track_button: Button
var _track_time: Label
var _tabs: TabBar
var _pages: Array = []    # per tab: the Control holding its rows
var _controls: Array = [] # per tab: Array of {name, effect, control, row, label}
var _arp_on := false
var _arp_timer := 0.0
var _arp_step := 0
var _arp_note := -1
var _overlay: Control
var _sound_reported := false
var _track_report := -1.0


func _ready() -> void:
	# The browser smoke test reuses this export: ?smoke=1 (or its probe
	# parameters) switches to the test harness scene instead.
	var query := ""
	if OS.has_feature("web"):
		query = str(JavaScriptBridge.eval("location.search"))
		for parameter in ["smoke=", "instances=", "synth="]:
			if query.contains(parameter):
				get_tree().change_scene_to_file.call_deferred("res://web_smoke.tscn")
				return

	_setup_audio()
	_sequencer = Sequencer.new()
	add_child(_sequencer)
	_sequencer.load_file(DEMO_TRACK)
	_sequencer.note_on.connect(_on_track_note_on)
	_sequencer.note_off.connect(_on_track_note_off)
	_sequencer.finished.connect(_on_track_finished)

	_build_ui()
	_build_boid()
	if OS.has_feature("web"):
		_build_start_overlay()
	# ?autoplay=1 starts the demo track (and &from=SECONDS skips ahead), so
	# browser_smoke.py can measure a busy passage.
	if query.contains("autoplay=1"):
		var from := 0.0
		for part in query.trim_prefix("?").split("&"):
			if part.begins_with("from="):
				from = float(part.trim_prefix("from="))
		_toggle_track.call_deferred(from)
	print("JSFX_PLAYGROUND_READY")


func _setup_audio() -> void:
	_mix_bus = AudioServer.bus_count
	AudioServer.add_bus(_mix_bus)
	AudioServer.set_bus_name(_mix_bus, MIX_BUS)
	for i in EFFECTS.size():
		var effect := AudioEffectJSFX.new()
		effect.script_path = EFFECTS[i].path
		AudioServer.add_bus_effect(_mix_bus, effect)
		AudioServer.set_bus_effect_enabled(_mix_bus, i, false)

	for config in LAYERS:
		var bus := AudioServer.bus_count
		AudioServer.add_bus(bus)
		AudioServer.set_bus_name(bus, config.name)
		AudioServer.set_bus_send(bus, MIX_BUS)
		AudioServer.set_bus_volume_db(bus, config.db)

		var stream := AudioStreamJSFX.new()
		stream.jsfx = load(config.path)
		var player := AudioStreamPlayer.new()
		player.stream = stream
		player.bus = config.name
		# Web defaults to sample playback, which skips Godot's mixer; JSFX
		# streams always mix through it.
		player.playback_type = AudioServer.PLAYBACK_TYPE_STREAM
		add_child(player)
		_layers.append({"effect": stream.jsfx, "bus": bus, "player": player,
				"enabled": false, "held": {}, "percussion": config.percussion})
		_set_layer_enabled(_layers.size() - 1, config.on)

	var synth: AudioEffectJSFX = _layers[0].effect
	synth.set("slider_4_release_ms", 500.0)
	# The piano's round robin cycles its four engines note by note.
	var piano: AudioEffectJSFX = _layers[1].effect
	piano.set("slider_30_round_robin_mode", 1)


func _set_layer_enabled(index: int, on: bool) -> void:
	var layer: Dictionary = _layers[index]
	if layer.enabled == on:
		return
	layer.enabled = on
	if on:
		# On Web this starts before the first click, and is heard once the
		# browser lets Godot resume its AudioContext.
		layer.player.play()
	else:
		# A stopped player costs no CPU; its script restarts fresh next time.
		for note in layer.held:
			_unlight(note)
		layer.held.clear()
		layer.effect.all_notes_off()
		layer.player.stop()


# --- Notes -------------------------------------------------------------------

# Every note source goes through here. Each layer counts how many sources
# hold a note: a doubled note restrikes, and stops when the last one lets go.
func _note_on(note: int, velocity := 100, percussion := false) -> void:
	for layer in _layers:
		if not layer.enabled or (percussion and not layer.percussion):
			continue
		layer.held[note] = layer.held.get(note, 0) + 1
		layer.effect.note_on(note, velocity)
		if not percussion:
			_light(note)


func _note_off(note: int, percussion := false) -> void:
	for layer in _layers:
		if not layer.held.has(note) or (percussion and not layer.percussion):
			continue
		layer.held[note] -= 1
		if layer.held[note] <= 0:
			layer.held.erase(note)
			layer.effect.note_off(note)
		if not percussion:
			_unlight(note)


func _light(note: int) -> void:
	_lit[note] = _lit.get(note, 0) + 1
	_piano.set_lit(note, true)


func _unlight(note: int) -> void:
	if not _lit.has(note):
		return
	_lit[note] -= 1
	if _lit[note] <= 0:
		_lit.erase(note)
		_piano.set_lit(note, false)


func _all_notes_off() -> void:
	for layer in _layers:
		layer.held.clear()
		layer.effect.all_notes_off()
	for note in _lit.keys():
		_piano.set_lit(note, false)
	_lit.clear()
	_key_notes.clear()
	_piano.release_all()
	_arp_note = -1


func _anything_sounding() -> bool:
	if _sequencer.playing or _arp_on:
		return true
	for layer in _layers:
		if not layer.held.is_empty():
			return true
	return false


func _on_track_note_on(channel: int, note: int, velocity: int) -> void:
	_note_on(note, velocity, channel == DRUM_CHANNEL)


func _on_track_note_off(channel: int, note: int) -> void:
	_note_off(note, channel == DRUM_CHANNEL)


func _toggle_track(from_seconds := 0.0) -> void:
	if _sequencer.playing:
		_sequencer.stop()
		_all_notes_off()
		_track_button.text = "Play demo track"
	else:
		_sequencer.play(from_seconds)
		_track_button.text = "Stop"
		_track_report = 4.0
		_dismiss_overlay()


func _on_track_finished() -> void:
	_all_notes_off()
	_track_button.text = "Play demo track"


# --- UI ----------------------------------------------------------------------

func _build_ui() -> void:
	set_anchors_preset(PRESET_FULL_RECT)

	var margin := MarginContainer.new()
	margin.set_anchors_preset(PRESET_FULL_RECT)
	for side in ["left", "right", "top", "bottom"]:
		margin.add_theme_constant_override("margin_" + side, 20)
	add_child(margin)

	var column := VBoxContainer.new()
	column.add_theme_constant_override("separation", 12)
	margin.add_child(column)

	var title := Label.new()
	title.text = "JSFX for Godot"
	title.add_theme_font_size_override("font_size", 28)
	column.add_child(title)
	var subtitle := Label.new()
	subtitle.text = ("REAPER JSFX instruments and effects running inside Godot's AudioServer. "
			+ "Every knob is a JSFX slider exposed as a Godot property. Click the boid.")
	subtitle.add_theme_color_override("font_color", MUTED)
	subtitle.autowrap_mode = TextServer.AUTOWRAP_WORD_SMART
	column.add_child(subtitle)

	var top := HBoxContainer.new()
	top.add_theme_constant_override("separation", 12)
	top.custom_minimum_size = Vector2(0, 300)
	column.add_child(top)
	_build_mixer(top)
	_build_options(top)

	_build_play_bar(column)

	_piano = Piano.new()
	_piano.first_note = 36
	_piano.key_count = 37
	_piano.custom_minimum_size = Vector2(0, 130)
	_piano.size_flags_vertical = SIZE_EXPAND_FILL
	_piano.note_pressed.connect(func(note: int) -> void: _note_on(note))
	_piano.note_released.connect(_note_off)
	column.add_child(_piano)

	var footer := RichTextLabel.new()
	footer.bbcode_enabled = true
	footer.fit_content = true
	footer.scroll_active = false
	footer.add_theme_color_override("default_color", MUTED)
	footer.text = ("Keys: A W S E D F T G Y H U J K O L P ;  ·  Z / X octave.   "
			+ "[color=#fba640]Unreal Piano, Cicada Swarm and Neighborhood[/color] were "
			+ "vibe-coded by Aya, a musician and REAPER enthusiast. Her non-AI music: "
			+ "GWID (vocal) and OK I DUNNO (instrumental), on Bandcamp and Spotify.")
	footer.meta_clicked.connect(func(meta: Variant) -> void: OS.shell_open(str(meta)))
	column.add_child(footer)

	_fps = Label.new()
	_fps.set_anchors_preset(PRESET_TOP_RIGHT)
	_fps.grow_horizontal = GROW_DIRECTION_BEGIN
	_fps.position = Vector2(-20, 16)
	_fps.horizontal_alignment = HORIZONTAL_ALIGNMENT_RIGHT
	_fps.add_theme_color_override("font_color", MUTED)
	add_child(_fps)


func _panel(parent: Control, title_text: String) -> VBoxContainer:
	var panel := PanelContainer.new()
	var style := StyleBoxFlat.new()
	style.bg_color = PANEL_COLOR
	style.set_corner_radius_all(8)
	style.set_content_margin_all(12)
	panel.add_theme_stylebox_override("panel", style)
	parent.add_child(panel)
	var rows := VBoxContainer.new()
	rows.add_theme_constant_override("separation", 4)
	panel.add_child(rows)
	if not title_text.is_empty():
		var title := Label.new()
		title.text = title_text
		title.add_theme_color_override("font_color", ACCENT)
		rows.add_child(title)
	return rows


# Layer switches, volumes and lo-fi factors, then the Mix bus effect switches.
func _build_mixer(parent: Control) -> void:
	var rows := _panel(parent, "Layers")
	rows.get_parent().custom_minimum_size = Vector2(430, 0)
	for i in LAYERS.size():
		var row := HBoxContainer.new()
		rows.add_child(row)
		var toggle := CheckButton.new()
		toggle.text = LAYERS[i].name
		toggle.button_pressed = LAYERS[i].on
		toggle.focus_mode = FOCUS_NONE
		toggle.custom_minimum_size = Vector2(170, 0)
		toggle.toggled.connect(func(on: bool) -> void: _set_layer_enabled(i, on))
		row.add_child(toggle)

		var volume := HSlider.new()
		volume.focus_mode = FOCUS_NONE
		volume.min_value = -40.0
		volume.max_value = 6.0
		volume.step = 0.5
		volume.value = LAYERS[i].db
		volume.size_flags_horizontal = SIZE_EXPAND_FILL
		volume.size_flags_vertical = SIZE_SHRINK_CENTER
		volume.tooltip_text = "Layer volume (dB)"
		var bus: int = _layers[i].bus
		volume.value_changed.connect(func(db: float) -> void:
			AudioServer.set_bus_volume_db(bus, db))
		row.add_child(volume)

		# downsample_factor runs the script at a fraction of the mix rate.
		var lofi := OptionButton.new()
		lofi.focus_mode = FOCUS_NONE
		lofi.tooltip_text = ("Lo-fi: run this script at 1/N of the mix rate. "
				+ "Saves CPU; changing it restarts the instrument.")
		for factor in LOFI_FACTORS:
			lofi.add_item("%d×" % factor if factor > 1 else "Hi-fi", factor)
		var effect: AudioEffectJSFX = _layers[i].effect
		lofi.item_selected.connect(func(index: int) -> void:
			effect.downsample_factor = lofi.get_item_id(index))
		row.add_child(lofi)

	var effects_title := Label.new()
	effects_title.text = "Mix effects"
	effects_title.add_theme_color_override("font_color", ACCENT)
	rows.add_child(effects_title)
	var effects_row := HFlowContainer.new()
	rows.add_child(effects_row)
	for i in EFFECTS.size():
		var toggle := CheckButton.new()
		toggle.text = EFFECTS[i].name
		toggle.focus_mode = FOCUS_NONE
		toggle.toggled.connect(func(on: bool) -> void:
			AudioServer.set_bus_effect_enabled(_mix_bus, i, on))
		effects_row.add_child(toggle)

	var hint := Label.new()
	hint.text = "Lo-fi runs a layer at 1/N of the mix rate: less CPU, grittier sound."
	hint.add_theme_color_override("font_color", MUTED)
	hint.add_theme_font_size_override("font_size", 13)
	rows.add_child(hint)


# One tab per layer and effect; only the visible tab's controls are shown,
# and those are the ones the boid changes.
func _build_options(parent: Control) -> void:
	var rows := _panel(parent, "")
	rows.get_parent().size_flags_horizontal = SIZE_EXPAND_FILL
	_tabs = TabBar.new()
	_tabs.focus_mode = FOCUS_NONE
	_tabs.clip_tabs = false
	rows.add_child(_tabs)

	var sources := []
	for i in LAYERS.size():
		sources.append([LAYERS[i].name, _layers[i].effect])
	for i in EFFECTS.size():
		sources.append([EFFECTS[i].name, AudioServer.get_bus_effect(_mix_bus, i)])

	for source in sources:
		_tabs.add_tab(source[0])
		var scroll := ScrollContainer.new()
		scroll.size_flags_vertical = SIZE_EXPAND_FILL
		scroll.horizontal_scroll_mode = ScrollContainer.SCROLL_MODE_DISABLED
		rows.add_child(scroll)
		var grid := GridContainer.new()
		grid.columns = 2
		grid.size_flags_horizontal = SIZE_EXPAND_FILL
		grid.add_theme_constant_override("h_separation", 24)
		grid.add_theme_constant_override("v_separation", 2)
		scroll.add_child(grid)
		_pages.append(scroll)
		_controls.append(_add_slider_controls(grid, source[1]))
	_tabs.tab_changed.connect(_show_tab)
	_show_tab(0)


func _show_tab(index: int) -> void:
	for i in _pages.size():
		_pages[i].visible = i == index


# One control per JSFX slider, read from the effect's property list: range
# sliders become HSliders (log-shaped ones exponential), enum sliders become
# option buttons. Changing a control sets the property, as gameplay code would.
func _add_slider_controls(parent: Control, effect: AudioEffectJSFX) -> Array:
	var made := []
	for property in effect.get_property_list():
		var name: String = property.name
		if not name.begins_with("slider_"):
			continue
		var row := HBoxContainer.new()
		row.size_flags_horizontal = SIZE_EXPAND_FILL
		parent.add_child(row)
		var label := Label.new()
		label.text = _pretty_name(name)
		label.tooltip_text = label.text
		label.mouse_filter = MOUSE_FILTER_PASS
		label.clip_text = true
		label.custom_minimum_size = Vector2(LABEL_WIDTH, 0)
		row.add_child(label)
		var entry := {"name": name, "effect": effect, "row": row, "label": label.text}
		made.append(entry)

		if property.hint == PROPERTY_HINT_ENUM:
			var options := OptionButton.new()
			options.focus_mode = FOCUS_NONE
			options.size_flags_horizontal = SIZE_EXPAND_FILL
			options.clip_text = true
			for item in String(property.hint_string).split(","):
				options.add_item(item.get_slice(":", 0), int(item.get_slice(":", 1)))
			options.select(options.get_item_index(int(effect.get(name))))
			options.item_selected.connect(func(index: int) -> void:
				effect.set(name, options.get_item_id(index)))
			row.add_child(options)
			entry.control = options
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
		entry.control = slider

		var value_label := Label.new()
		value_label.custom_minimum_size = Vector2(52, 0)
		value_label.horizontal_alignment = HORIZONTAL_ALIGNMENT_RIGHT
		value_label.add_theme_color_override("font_color", MUTED)
		value_label.text = _format_value(slider.value)
		row.add_child(value_label)

		slider.value_changed.connect(func(value: float) -> void:
			effect.set(name, value)
			value_label.text = _format_value(value))
	return made


func _build_play_bar(column: VBoxContainer) -> void:
	var bar := HBoxContainer.new()
	bar.add_theme_constant_override("separation", 14)
	column.add_child(bar)

	_track_button = Button.new()
	_track_button.text = "Play demo track"
	_track_button.focus_mode = FOCUS_NONE
	_track_button.custom_minimum_size = Vector2(150, 0)
	_track_button.pressed.connect(func() -> void: _toggle_track())
	bar.add_child(_track_button)
	_track_time = Label.new()
	_track_time.custom_minimum_size = Vector2(220, 0)
	_track_time.add_theme_color_override("font_color", MUTED)
	bar.add_child(_track_time)

	var mod := _labeled_slider(bar, "Mod", 0.0, 127.0, 1.0)
	mod.value_changed.connect(func(value: float) -> void:
		for layer in _layers:
			layer.effect.control_change(1, int(value)))
	# Pitch bend springs back to the centre when let go, like a real wheel.
	_bend = _labeled_slider(bar, "Bend", -1.0, 1.0, 0.01)
	_bend.value_changed.connect(func(value: float) -> void:
		for layer in _layers:
			layer.effect.pitch_bend(value))
	_bend.drag_ended.connect(func(_changed: bool) -> void: _bend.value = 0.0)

	var down := Button.new()
	down.text = " - "
	down.focus_mode = FOCUS_NONE
	down.pressed.connect(_shift_octave.bind(-1))
	bar.add_child(down)
	_octave_label = Label.new()
	_octave_label.custom_minimum_size = Vector2(32, 0)
	_octave_label.horizontal_alignment = HORIZONTAL_ALIGNMENT_CENTER
	bar.add_child(_octave_label)
	var up := Button.new()
	up.text = " + "
	up.focus_mode = FOCUS_NONE
	up.pressed.connect(_shift_octave.bind(1))
	bar.add_child(up)
	_shift_octave(0)

	var arp := CheckButton.new()
	arp.text = "Arpeggiator"
	arp.focus_mode = FOCUS_NONE
	arp.toggled.connect(_set_arpeggiator)
	bar.add_child(arp)

	_meter = ProgressBar.new()
	_meter.min_value = -60.0
	_meter.max_value = 0.0
	_meter.show_percentage = false
	_meter.size_flags_horizontal = SIZE_EXPAND_FILL
	_meter.size_flags_vertical = SIZE_SHRINK_CENTER
	_meter.custom_minimum_size = Vector2(80, 10)
	var fill := StyleBoxFlat.new()
	fill.bg_color = ACCENT
	_meter.add_theme_stylebox_override("fill", fill)
	bar.add_child(_meter)


func _labeled_slider(parent: Control, text: String, min_value: float,
		max_value: float, step: float) -> HSlider:
	var label := Label.new()
	label.text = text
	parent.add_child(label)
	var slider := HSlider.new()
	slider.focus_mode = FOCUS_NONE
	slider.min_value = min_value
	slider.max_value = max_value
	slider.step = step
	slider.custom_minimum_size = Vector2(90, 0)
	slider.size_flags_vertical = SIZE_SHRINK_CENTER
	parent.add_child(slider)
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


# --- Boid ----------------------------------------------------------------------

func _build_boid() -> void:
	var layer := CanvasLayer.new()
	layer.layer = 5
	add_child(layer)
	_boid = Boid.new()
	layer.add_child(_boid)
	_boid.clicked.connect(_on_boid_clicked)


# Change one random parameter of the visible tab, and make it loud and
# visible: the slider glides there, its row flashes, the boid punches and
# names the change. With nothing playing, a short note makes it audible.
func _on_boid_clicked() -> void:
	var color := _boid.punch()
	var candidates := []
	for entry in _controls[_tabs.current_tab]:
		var skip := false
		for word in BOID_SKIP:
			if entry.name.contains(word):
				skip = true
		if not skip:
			candidates.append(entry)
	if candidates.is_empty():
		return
	var entry: Dictionary = candidates.pick_random()
	var text := ""
	if entry.control is OptionButton:
		var options: OptionButton = entry.control
		var index := randi() % options.item_count
		if options.item_count > 1 and index == options.selected:
			index = (index + 1) % options.item_count
		options.select(index)
		entry.effect.set(entry.name, options.get_item_id(index))
		text = "%s: %s" % [entry.label, options.get_item_text(index)]
	else:
		var slider: HSlider = entry.control
		# Stay inside the middle of the range, so a click never lands on a
		# setting that silences the instrument.
		var t := randf_range(0.1, 0.9)
		var target: float
		if slider.exp_edit and slider.min_value > 0.0:
			target = slider.min_value * pow(slider.max_value / slider.min_value, t)
		else:
			target = lerpf(slider.min_value, slider.max_value, t)
		create_tween().tween_property(slider, "value", target, 0.15) \
				.set_trans(Tween.TRANS_QUAD).set_ease(Tween.EASE_OUT)
		if slider.step > 0.0:
			target = snappedf(target, slider.step)
		text = "%s: %s" % [entry.label, _format_value(target)]
	_boid.show_label(text, color)

	var row: Control = entry.row
	row.modulate = color.lightened(0.3) * 1.6
	create_tween().tween_property(row, "modulate", Color.WHITE, 0.5)

	if not _anything_sounding():
		var note: int = [48, 55, 60, 64, 67, 72].pick_random()
		_note_on(note, 110)
		get_tree().create_timer(0.3).timeout.connect(func() -> void: _note_off(note))


# --- Input and per-frame -------------------------------------------------------

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
			_note_on(_key_notes[key])
		elif not event.pressed and _key_notes.has(key):
			_note_off(_key_notes[key])
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
		_note_off(_arp_note)
		_arp_note = -1


func _notification(what: int) -> void:
	# Keys released while the window is unfocused never send key-up events.
	if what == NOTIFICATION_APPLICATION_FOCUS_OUT and _piano != null:
		for key in _key_notes:
			_note_off(_key_notes[key])
		_key_notes.clear()
		_piano.release_all()


func _process(delta: float) -> void:
	if _layers.is_empty():
		return
	_fps.text = "%d FPS" % Engine.get_frames_per_second()

	if _arp_on:
		_arp_timer -= delta
		if _arp_timer <= 0.0:
			_arp_timer += ARP_STEP_SECONDS
			var chord: Array = PROGRESSION[(_arp_step / ARP_PATTERN.size()) % PROGRESSION.size()]
			if _arp_note >= 0:
				_note_off(_arp_note)
			_arp_note = chord[ARP_PATTERN[_arp_step % ARP_PATTERN.size()]]
			_note_on(_arp_note)
			_arp_step += 1

	if _sequencer.playing:
		_track_time.text = "%s / %s  Rossini, Barber of Seville" % [
				_clock(_sequencer.position), _clock(_sequencer.midi.length_seconds)]
	else:
		_track_time.text = "Rossini, Barber of Seville"

	var peak := maxf(AudioServer.get_bus_peak_volume_left_db(_mix_bus, 0),
			AudioServer.get_bus_peak_volume_right_db(_mix_bus, 0))
	_meter.value = maxf(peak, -60.0)
	# Markers for tools/browser_smoke.py --playground.
	if not _sound_reported and peak > -40.0:
		_sound_reported = true
		print("JSFX_PLAYGROUND_SOUND peak_db=", peak)
	if _track_report > 0.0 and _sequencer.playing:
		_track_report -= delta
		if _track_report <= 0.0:
			_track_report = 4.0
			print("JSFX_TRACK_PLAYING position=%.1f peak_db=%.1f fps=%d" % [
					_sequencer.position, peak, Engine.get_frames_per_second()])


static func _clock(seconds: float) -> String:
	return "%d:%02d" % [int(seconds) / 60, int(seconds) % 60]
