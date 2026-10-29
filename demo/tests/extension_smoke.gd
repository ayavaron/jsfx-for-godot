extends SceneTree

# Headless Phase 2 smoke test. Verifies that the GDExtension registers
# AudioEffectJSFX, loads/parses a JSFX file, exposes the Phase 2 methods and
# properties, and can instantiate the effect on the Master bus.
#
#   godot --headless --path demo --script res://tests/extension_smoke.gd
#
# Prints JSFX_SMOKE_OK on success and exits with the GDScript quit code.

func _initialize() -> void:
	var failures: Array[String] = []

	var effect := AudioEffectJSFX.new()
	if effect == null:
		failures.append("AudioEffectJSFX.new() returned null")
		_finish(failures)
		return

	effect.script_path = "res://gain.jsfx"
	if effect.get_script_path() != "res://gain.jsfx":
		failures.append("script_path round-trip failed")

	var default_gain := effect.get_slider(1)
	if absf(default_gain - 0.5) > 1e-9:
		failures.append("default slider was %s, expected 0.5" % default_gain)

	if not effect.set_slider(1, 0.25):
		failures.append("set_slider(1) returned false")
	if absf(effect.get_slider(1) - 0.25) > 1e-9:
		failures.append("slider value did not round-trip")

	# Story 3.2: sliders are exposed as animatable Godot properties.
	var property_names: Array[String] = []
	for property in effect.get_property_list():
		property_names.append(String(property["name"]))
	if not property_names.has("slider_1_gain"):
		failures.append("slider_1_gain is missing from get_property_list()")
	if not property_names.has("script_path"):
		failures.append("built-in script_path disappeared from the property list")

	effect.set("slider_1_gain", 0.3)
	if absf(float(effect.get("slider_1_gain")) - 0.3) > 1e-9:
		failures.append("slider_1_gain did not round-trip through set/get")

	# Dynamic properties must serialize into .tres and restore on load.
	var save_path := "user://jsfx_effect_smoke.tres"
	var save_error := ResourceSaver.save(effect, save_path)
	if save_error != OK:
		failures.append("ResourceSaver.save failed: %s" % error_string(save_error))
	else:
		var saved_effect := ResourceLoader.load(save_path) as AudioEffectJSFX
		if saved_effect == null:
			failures.append("ResourceLoader.load did not return AudioEffectJSFX")
		elif absf(saved_effect.get_slider(1) - 0.3) > 1e-9:
			failures.append("serialized slider value did not round-trip")
		DirAccess.remove_absolute(ProjectSettings.globalize_path(save_path))

	# Story 3.1: the ResourceFormatLoader makes .jsfx files loadable resources.
	var loaded_jsfx := load("res://gain.jsfx") as AudioEffectJSFX
	if loaded_jsfx == null:
		failures.append("load(\"res://gain.jsfx\") did not return AudioEffectJSFX")
	else:
		if loaded_jsfx.get_script_path() != "res://gain.jsfx":
			failures.append("JSFX loader stored path %s" % loaded_jsfx.get_script_path())
		if absf(loaded_jsfx.get_slider(1) - 0.5) > 1e-9:
			failures.append("JSFX loader default slider was not 0.5")

	effect.downsample_factor = 4
	if effect.downsample_factor != 4:
		failures.append("downsample_factor round-trip failed")
	effect.force_mono = true
	if not effect.force_mono:
		failures.append("force_mono round-trip failed")

	# This calls AudioEffectJSFX::_instantiate() and compiles EEL2.
	AudioServer.add_bus_effect(0, effect)
	if AudioServer.get_bus_effect_count(0) < 1:
		failures.append("effect was not added to the Master bus")

	# Invalid updates must be rejected without crashing.
	if effect.set_slider(99, 1.0):
		failures.append("set_slider accepted an unknown index")
	if effect.set_slider(1, NAN):
		failures.append("set_slider accepted NaN")

	AudioServer.remove_bus_effect(0, 0)
	_check_hot_reload(failures)
	_finish(failures)


func _check_hot_reload(failures: Array[String]) -> void:
	var hot_path := "user://jsfx_hot_reload_smoke.jsfx"
	var version_one := (
		"slider1:0.25<0,1,0.01>Gain\n"
		+ "@sample\nspl0 *= slider1;\nspl1 *= slider1;\n"
	)
	var version_two := (
		"slider1:0.75<0,1,0.01>Level\n"
		+ "@sample\nspl0 *= slider1;\nspl1 *= slider1;\n"
	)

	if not _write_text(hot_path, version_one):
		failures.append("could not write %s" % hot_path)
		return

	var hot_effect := AudioEffectJSFX.new()
	hot_effect.script_path = hot_path
	if absf(hot_effect.get_slider(1) - 0.25) > 1e-9:
		failures.append("hot-reload script default was not 0.25")
	hot_effect.set_slider(1, 0.5)

	# Instantiate a live instance, then swap it to the new script.
	AudioServer.add_bus_effect(0, hot_effect)
	if not _write_text(hot_path, version_two):
		failures.append("could not rewrite %s" % hot_path)
	else:
		if not hot_effect.reload_script():
			failures.append("reload_script() returned false for a valid script")
		else:
			if absf(hot_effect.get_slider(1) - 0.5) > 1e-9:
				failures.append("hot reload did not preserve the set slider value")

			var reloaded_properties: Array[String] = []
			for property in hot_effect.get_property_list():
				reloaded_properties.append(String(property["name"]))
			if not reloaded_properties.has("slider_1_level"):
				failures.append("hot reload did not expose the new slider name")
			if reloaded_properties.has("slider_1_gain"):
				failures.append("hot reload kept the old slider name")

			hot_effect.set("slider_1_level", 0.6)
			if absf(float(hot_effect.get("slider_1_level")) - 0.6) > 1e-9:
				failures.append("new slider property did not round-trip")

		# A broken save must keep the previous script running (an ERR_PRINT is
		# expected here).
		if not _write_text(hot_path, "slider1:0<0,1,1>Bad\n@sample\nspl0 = ;\n"):
			failures.append("could not write the broken hot-reload script")
		elif hot_effect.reload_script():
			failures.append("reload_script() accepted a broken script")
		elif absf(float(hot_effect.get("slider_1_level")) - 0.6) > 1e-9:
			failures.append("broken reload did not keep the previous script")

		# The editor file watcher polls mtime and calls reload_script(). Exercise
		# the same entry point manually after a filesystem-visible delay.
		OS.delay_msec(1200)
		if not _write_text(hot_path, "slider1:0.75<0,1,0.01>Level2\n@sample\nspl0 *= slider1;\nspl1 *= slider1;\n"):
			failures.append("could not write the polled hot-reload script")
		else:
			for _i in range(15):
				hot_effect.call("_poll_script_file")

			var polled_properties: Array[String] = []
			for property in hot_effect.get_property_list():
				polled_properties.append(String(property["name"]))
			if not polled_properties.has("slider_1_level2"):
				failures.append("mtime polling did not reload the changed script")

	AudioServer.remove_bus_effect(0, 0)
	DirAccess.remove_absolute(ProjectSettings.globalize_path(hot_path))


func _write_text(path: String, text: String) -> bool:
	var file := FileAccess.open(path, FileAccess.WRITE)
	if file == null:
		return false
	file.store_string(text)
	file.close()
	return true


func _finish(failures: Array[String]) -> void:
	if failures.is_empty():
		print("JSFX_SMOKE_OK")
		quit(0)
		return

	for failure in failures:
		printerr("JSFX_SMOKE_FAIL: " + failure)
	quit(1)
