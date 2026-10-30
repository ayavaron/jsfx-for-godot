extends GutTest

# Phase 3 GUT coverage, including the Story 6.2 automation stress case.
# Run after installing the addon with tools/fetch_gut.ps1:
#   godot --headless --path demo -s addons/gut/gut_cmdln.gd \
#     -gdir=res://tests/unit -gexit

const GAIN_SCRIPT := "res://gain.jsfx"


func _has_property(object: Object, property_name: String) -> bool:
	for property in object.get_property_list():
		if String(property["name"]) == property_name:
			return true
	return false


func test_slider_properties_are_exposed_and_serializable() -> void:
	var effect := AudioEffectJSFX.new()
	effect.script_path = GAIN_SCRIPT

	assert_true(_has_property(effect, "slider_1_gain"),
		"slider_1_gain should be in the property list")
	assert_almost_eq(effect.get_slider(1), 0.5, 1e-6)

	effect.set("slider_1_gain", 0.25)
	assert_almost_eq(float(effect.get("slider_1_gain")), 0.25, 1e-6)

	var save_path := "user://gut_jsfx_effect.tres"
	assert_eq(ResourceSaver.save(effect, save_path), OK)

	var loaded := ResourceLoader.load(save_path) as AudioEffectJSFX
	assert_not_null(loaded)
	assert_almost_eq(loaded.get_slider(1), 0.25, 1e-6)
	DirAccess.remove_absolute(ProjectSettings.globalize_path(save_path))


func test_jsfx_files_load_through_the_resource_loader() -> void:
	var loaded := load(GAIN_SCRIPT) as AudioEffectJSFX
	assert_not_null(loaded, ".jsfx should load as AudioEffectJSFX")
	assert_almost_eq(loaded.get_slider(1), 0.5, 1e-6)


func test_threaded_resource_loading_defers_compilation_to_main() -> void:
	var path := "user://gut_threaded_gain.jsfx"
	var file := FileAccess.open(path, FileAccess.WRITE)
	assert_not_null(file)
	file.store_string("slider1:0.25<0,1,0.01>Gain\n@sample\nspl0 *= slider1;\nspl1 *= slider1;\n")
	file.close()

	assert_eq(ResourceLoader.load_threaded_request(path), OK)

	var loaded: AudioEffectJSFX = null
	var frames := 0
	while frames < 600:
		var status := ResourceLoader.load_threaded_get_status(path)
		if status == ResourceLoader.THREAD_LOAD_LOADED:
			loaded = ResourceLoader.load_threaded_get(path) as AudioEffectJSFX
			break
		if status == ResourceLoader.THREAD_LOAD_FAILED:
			break
		await get_tree().process_frame
		frames += 1

	assert_not_null(loaded, "threaded load did not return AudioEffectJSFX")
	if loaded != null:
		assert_eq(loaded.get_script_path(), path)
		# The worker stores only the path; this first main-thread access parses
		# and compiles it.
		assert_almost_eq(loaded.get_slider(1), 0.25, 1e-6)
		assert_true(_has_property(loaded, "slider_1_gain"))

	DirAccess.remove_absolute(ProjectSettings.globalize_path(path))


func test_threaded_tres_loading_preserves_slider_values() -> void:
	var effect := AudioEffectJSFX.new()
	effect.script_path = GAIN_SCRIPT
	effect.set_slider(1, 0.3)

	var path := "user://gut_threaded_effect.tres"
	assert_eq(ResourceSaver.save(effect, path), OK)
	assert_eq(ResourceLoader.load_threaded_request(path), OK)

	var loaded: AudioEffectJSFX = null
	var frames := 0
	while frames < 600:
		var status := ResourceLoader.load_threaded_get_status(path)
		if status == ResourceLoader.THREAD_LOAD_LOADED:
			loaded = ResourceLoader.load_threaded_get(path) as AudioEffectJSFX
			break
		if status == ResourceLoader.THREAD_LOAD_FAILED:
			break
		await get_tree().process_frame
		frames += 1

	assert_not_null(loaded, "threaded .tres load did not return AudioEffectJSFX")
	if loaded != null:
		assert_almost_eq(loaded.get_slider(1), 0.3, 1e-6)

	DirAccess.remove_absolute(ProjectSettings.globalize_path(path))


func test_audio_frames_are_processed() -> void:
	if DisplayServer.get_name() == "headless":
		pass_test("Headless audio does not mix; covered by browser/real-device runs")
		return

	var effect := AudioEffectJSFX.new()
	effect.script_path = GAIN_SCRIPT
	var bus_effect_index := AudioServer.get_bus_effect_count(0)
	AudioServer.add_bus_effect(0, effect)

	for _i in range(30):
		await get_tree().process_frame

	var frames := effect.get_processed_frames()
	AudioServer.remove_bus_effect(0, bus_effect_index)
	assert_gt(frames, 0, "AudioEffectJSFXInstance::_process was not dispatched")


func test_rapid_tween_and_parameter_spew_do_not_stall() -> void:
	var effect := AudioEffectJSFX.new()
	effect.script_path = GAIN_SCRIPT

	var bus_effect_index := AudioServer.get_bus_effect_count(0)
	AudioServer.add_bus_effect(0, effect)
	assert_eq(AudioServer.get_bus_effect_count(0), bus_effect_index + 1)

	# Story 6.2: Tween-driven automation for several frames.
	var tween := create_tween()
	tween.tween_property(effect, "slider_1_gain", 1.0, 0.1).from(0.0)
	var frames := 0
	while tween.is_running() and frames < 600:
		await get_tree().process_frame
		frames += 1

	assert_false(tween.is_running(), "the tween stalled")
	assert_lt(frames, 600, "automation did not finish")
	assert_almost_eq(float(effect.get("slider_1_gain")), 1.0, 0.01)

	# Burst of main-thread writes; the bounded queue may drop, but must not
	# block, deadlock or throw. The elapsed-time assertion is deliberately loose.
	var start := Time.get_ticks_msec()
	for i in range(5000):
		effect.set("slider_1_gain", float(i % 101) / 100.0)
	var elapsed := Time.get_ticks_msec() - start
	assert_lt(elapsed, 5000, "parameter burst stalled the main thread")

	AudioServer.remove_bus_effect(0, bus_effect_index)
	assert_eq(AudioServer.get_bus_effect_count(0), bus_effect_index)


func _property(object: Object, property_name: String) -> Dictionary:
	for property in object.get_property_list():
		if String(property["name"]) == property_name:
			return property
	return {}


func test_modern_slider_syntax_maps_to_godot_hints() -> void:
	var effect := AudioEffectJSFX.new()
	effect.script_path = "res://effects/tremolo.jsfx"

	# Named sliders keep the slider_<n>_<description> property name.
	var rate := _property(effect, "slider_1_rate_hz")
	assert_eq(rate.get("type"), TYPE_FLOAT)
	assert_eq(rate.get("hint"), PROPERTY_HINT_RANGE)
	assert_string_contains(String(rate.get("hint_string")), "exp")

	# Enum sliders become INT properties with labels.
	var shape := _property(effect, "slider_3_shape")
	assert_eq(shape.get("type"), TYPE_INT)
	assert_eq(shape.get("hint"), PROPERTY_HINT_ENUM)
	assert_eq(String(shape.get("hint_string")), "Sine:0,Triangle:1,Square:2")
	effect.set("slider_3_shape", 2)
	assert_eq(effect.get("slider_3_shape"), 2)

	# Revert-to-default comes from the JSFX header.
	assert_true(effect.property_can_revert("slider_2_depth"))
	assert_almost_eq(float(effect.property_get_revert("slider_2_depth")), 0.5, 1e-6)


func test_imports_resolve_relative_to_the_script() -> void:
	# tremolo.jsfx calls lfo_shape() from lib/lfo.jsfx-inc. If the import did
	# not resolve, compilation fails and the effect falls back to passthrough.
	var effect := AudioEffectJSFX.new()
	effect.script_path = "res://effects/tremolo.jsfx"
	AudioServer.add_bus_effect(0, effect)
	var index := AudioServer.get_bus_effect_count(0) - 1
	assert_not_null(AudioServer.get_bus_effect_instance(0, index))
	assert_true(effect.reload_script(), "tremolo should compile with its import")
	AudioServer.remove_bus_effect(0, index)


func test_instrument_stream_receives_midi_while_playing() -> void:
	var stream := AudioStreamJSFX.new()
	stream.jsfx = load("res://instruments/simple_synth.jsfx")
	var keys: AudioEffectJSFX = stream.jsfx
	assert_false(keys.note_on(60), "nothing is playing yet to receive MIDI")

	var player := AudioStreamPlayer.new()
	player.stream = stream
	add_child_autofree(player)
	player.play()
	var playback := player.get_stream_playback()
	assert_not_null(playback)
	assert_eq(playback.get_class(), "AudioStreamPlaybackJSFX")

	assert_true(keys.note_on(60, 100))
	assert_true(keys.note_off(60))
	assert_true(keys.control_change(1, 64))
	assert_true(keys.pitch_bend(0.25))
	assert_true(keys.all_notes_off())
	assert_false(keys.send_midi(0x10, 0, 0), "status bytes start at 0x80")

	# Instrument sliders are the wrapped effect's properties.
	keys.set("slider_5_cutoff_hz", 800.0)
	assert_almost_eq(keys.get_slider(5), 800.0, 1e-6)
	player.stop()
