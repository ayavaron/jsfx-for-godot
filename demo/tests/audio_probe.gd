extends SceneTree

# Real-audio probe. Needs a real audio driver (not --headless's dummy):
#   godot --path demo --audio-driver WASAPI --script res://tests/audio_probe.gd
#
# 1. Accuracy: a sine through a JSFX gain, captured after the effect.
# 2. Hot reload under load: hundreds of recompiles while audio plays.
# 3. Instantiation cost on the main thread and many live instances.
# Prints AUDIO_PROBE_OK on success, AUDIO_PROBE_FAIL otherwise.

const TONE_HZ := 440.0
const AMPLITUDE := 0.5
const RELOAD_PATH := "user://audio_probe_reload.jsfx"

var _playback: AudioStreamGeneratorPlayback
var _phase := 0.0
var _mix_rate := 48000.0
var _failures: PackedStringArray = []


func _initialize() -> void:
	_run.call_deferred()


func _check(ok: bool, message: String) -> void:
	print(("  ok   " if ok else "  FAIL ") + message)
	if not ok:
		_failures.append(message)


func _feed() -> void:
	var frames := _playback.get_frames_available()
	for _i in frames:
		var s := sin(_phase) * AMPLITUDE
		_phase = fmod(_phase + TAU * TONE_HZ / _mix_rate, TAU)
		_playback.push_frame(Vector2(s, s))


func _wait(seconds: float) -> void:
	var until := Time.get_ticks_msec() + int(seconds * 1000.0)
	while Time.get_ticks_msec() < until:
		_feed()
		await process_frame


func _capture_rms(capture: AudioEffectCapture, seconds: float) -> float:
	capture.clear_buffer()
	await _wait(seconds)
	var frames := capture.get_buffer(capture.get_frames_available())
	if frames.is_empty():
		return -1.0
	var sum := 0.0
	for f in frames:
		sum += f.x * f.x
	return sqrt(sum / frames.size())


func _write_gain_script(gain: float) -> void:
	var file := FileAccess.open(RELOAD_PATH, FileAccess.WRITE)
	file.store_string("slider1:%f<0,1,0.01>Gain\n@sample\nspl0 *= slider1;\nspl1 *= slider1;\n" % gain)
	file.close()


func _run() -> void:
	_mix_rate = AudioServer.get_mix_rate()
	print("AUDIO_PROBE driver=", AudioServer.get_driver_name() if AudioServer.has_method("get_driver_name") else "?",
		" mix_rate=", _mix_rate)

	var bus := AudioServer.bus_count
	AudioServer.add_bus(bus)
	AudioServer.set_bus_name(bus, "Probe")

	var gain := AudioEffectJSFX.new()
	gain.script_path = "res://gain.jsfx"
	gain.set("slider_1_gain", 0.5)
	AudioServer.add_bus_effect(bus, gain)
	var capture := AudioEffectCapture.new()
	capture.buffer_length = 2.0
	AudioServer.add_bus_effect(bus, capture)

	var generator := AudioStreamGenerator.new()
	generator.mix_rate = _mix_rate
	generator.buffer_length = 0.25
	var player := AudioStreamPlayer.new()
	player.stream = generator
	player.bus = "Probe"
	root.add_child(player)
	player.play()
	_playback = player.get_stream_playback()
	await _wait(0.3)

	# 1. Accuracy ----------------------------------------------------------
	var expected := AMPLITUDE / sqrt(2.0)
	var rms := await _capture_rms(capture, 0.5)
	_check(absf(rms - expected * 0.5) < 0.01,
		"gain 0.5: rms %.4f (expected %.4f)" % [rms, expected * 0.5])
	gain.set("slider_1_gain", 0.25)
	await _wait(0.1)
	rms = await _capture_rms(capture, 0.5)
	_check(absf(rms - expected * 0.25) < 0.01,
		"gain 0.25 via property: rms %.4f (expected %.4f)" % [rms, expected * 0.25])

	# 2. Hot reload under load ----------------------------------------------
	_write_gain_script(0.5)
	gain.script_path = RELOAD_PATH
	var reloads := 0
	var reload_ok := true
	var start := Time.get_ticks_msec()
	while Time.get_ticks_msec() - start < 3000:
		_write_gain_script(0.5 if reloads % 2 == 0 else 0.25)
		reload_ok = gain.reload_script() and reload_ok
		reloads += 1
		_feed()
		await process_frame
	_write_gain_script(0.75)
	reload_ok = gain.reload_script() and reload_ok
	await _wait(0.2)
	rms = await _capture_rms(capture, 0.5)
	_check(reload_ok, "%d hot reloads while playing" % reloads)
	_check(absf(rms - expected * 0.75) < 0.01,
		"after reload, script default 0.75 applies: rms %.4f" % rms)

	# 3. Downsampled scripts see the reduced sample rate ---------------------
	var file := FileAccess.open(RELOAD_PATH, FileAccess.WRITE)
	file.store_string("@sample
spl0 = spl1 = srate / 100000;
")
	file.close()
	gain.reload_script()
	gain.downsample_factor = 2
	await _wait(0.2)
	rms = await _capture_rms(capture, 0.3)
	_check(absf(rms - _mix_rate / 2.0 / 100000.0) < 0.001,
		"downsample_factor 2 runs the script at srate %.0f" % (rms * 100000.0))
	gain.downsample_factor = 1
	await _wait(0.2)
	rms = await _capture_rms(capture, 0.3)
	_check(absf(rms - _mix_rate / 100000.0) < 0.001,
		"downsample_factor 1 restores srate %.0f" % (rms * 100000.0))

	# 4. Instantiation cost and many instances ------------------------------
	var instantiate_ms: Array[float] = []
	var buses: Array[int] = []
	for i in 16:
		var b := AudioServer.bus_count
		AudioServer.add_bus(b)
		AudioServer.set_bus_send(b, "Probe")
		# Parse + compile happen when script_path is set; time both steps.
		var t0 := Time.get_ticks_usec()
		var lowpass := AudioEffectJSFX.new()
		lowpass.script_path = "res://effects/lowpass.jsfx"
		AudioServer.add_bus_effect(b, lowpass)
		instantiate_ms.append((Time.get_ticks_usec() - t0) / 1000.0)
		buses.append(b)
	instantiate_ms.sort()
	print("  load + compile + add_bus_effect: median %.2f ms, max %.2f ms" %
		[instantiate_ms[instantiate_ms.size() / 2], instantiate_ms[-1]])

	var before: int = gain.get_processed_frames()
	await _wait(1.0)
	var produced: int = gain.get_processed_frames() - before
	_check(produced > _mix_rate * 0.8,
		"audio keeps up with 17 live JSFX buses: %d frames in 1 s" % produced)

	if _failures.is_empty():
		print("AUDIO_PROBE_OK")
	else:
		print("AUDIO_PROBE_FAIL ", _failures)
	_playback = null
	quit(0 if _failures.is_empty() else 1)
