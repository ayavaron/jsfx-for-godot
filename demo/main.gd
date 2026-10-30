extends Node

# Web + desktop smoke demo:
#   - the Master bus runs a JSFX gain effect,
#   - a generated 220 Hz tone is played through it,
#   - a Tween automates the generated `slider_1_gain` property.
# Browser autoplay policies start Web Audio suspended, so on Web the tone is
# started shortly after the first mouse/key input.

const SCRIPT_PATH := "res://gain.jsfx"
const TONE_HZ := 220.0
const MIX_RATE := 44100.0

var _effect: AudioEffectJSFX
var _player: AudioStreamPlayer
var _playback: AudioStreamGeneratorPlayback
var _phase := 0.0
var _audio_started := false
var _start_delay := -1.0
var _frames_reported := false


func _ready() -> void:
	var effect := AudioEffectJSFX.new()
	effect.script_path = SCRIPT_PATH
	effect.set("slider_1_gain", 0.2)
	AudioServer.add_bus_effect(0, effect)
	_effect = effect

	var generator := AudioStreamGenerator.new()
	generator.mix_rate = MIX_RATE
	generator.buffer_length = 0.2
	_player = AudioStreamPlayer.new()
	_player.stream = generator
	add_child(_player)

	if not OS.has_feature("web"):
		_start_audio()

	var tween := create_tween().set_loops()
	tween.tween_property(_effect, "slider_1_gain", 0.5, 2.0).from(0.05)

	# Memory probe: `?instances=N` in a Web export adds N lowpass effects, each
	# on its own bus, so browser_smoke.py can compare heap sizes.
	var extra := 0
	if OS.has_feature("web"):
		extra = int(str(JavaScriptBridge.eval(
			"new URLSearchParams(location.search).get('instances') || '0'")))
	for i in extra:
		var bus := AudioServer.bus_count
		AudioServer.add_bus(bus)
		var lowpass := AudioEffectJSFX.new()
		lowpass.script_path = "res://effects/lowpass.jsfx"
		AudioServer.add_bus_effect(bus, lowpass)
	if extra > 0:
		print("JSFX_EXTRA_INSTANCES=", extra)

	print("JSFX_WEB_READY")


func _start_audio() -> void:
	if _audio_started:
		return
	_audio_started = true
	_player.play()
	_playback = _player.get_stream_playback() as AudioStreamGeneratorPlayback
	print("JSFX_AUDIO_STARTED playback=", _playback != null)


func _input(event: InputEvent) -> void:
	var pressed_gesture := false
	if event is InputEventMouseButton:
		pressed_gesture = event.pressed
	elif event is InputEventKey:
		pressed_gesture = event.pressed
	if pressed_gesture and not _audio_started and _start_delay < 0.0:
		# Let the browser's AudioContext resume from the gesture first.
		_start_delay = 0.25


func _process(delta: float) -> void:
	if _start_delay >= 0.0 and not _audio_started:
		_start_delay -= delta
		if _start_delay <= 0.0:
			_start_audio()

	if _playback != null:
		var available := _playback.get_frames_available()
		for _i in available:
			var sample := sin(_phase) * 0.25
			_phase = fmod(_phase + TAU * TONE_HZ / MIX_RATE, TAU)
			_playback.push_frame(Vector2(sample, sample))

	if _frames_reported or _effect == null:
		return
	var frames := _effect.get_processed_frames()
	if frames > 0:
		_frames_reported = true
		print(
			"JSFX_WEB_PROCESSED=", frames,
			" peak_db=", AudioServer.get_bus_peak_volume_left_db(0, 0)
		)
