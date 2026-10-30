extends Node

# Plays the demo JSFX synth as an instrument (AudioStreamJSFX).
#   - Keys A S D F G H J K play a C major scale; hold for sustain.
#   - Mouse X sweeps the mod wheel (filter), mouse Y bends pitch.
#   - With no keys pressed for 3 seconds, a slow arpeggio plays by itself.
# Run with: godot --path demo res://synth_demo.tscn

const KEYS := [KEY_A, KEY_S, KEY_D, KEY_F, KEY_G, KEY_H, KEY_J, KEY_K]
const SCALE := [60, 62, 64, 65, 67, 69, 71, 72]
const ARPEGGIO := [48, 55, 60, 64, 67, 72, 67, 64]

var _synth: AudioEffectJSFX
var _idle := 0.0
var _step_timer := 0.0
var _step := 0
var _arp_note := -1


func _ready() -> void:
	var stream := AudioStreamJSFX.new()
	stream.jsfx = load("res://instruments/simple_synth.jsfx")
	_synth = stream.jsfx
	_synth.set("slider_4_release_ms", 600.0)

	var player := AudioStreamPlayer.new()
	player.stream = stream
	add_child(player)
	player.play()

	var label := Label.new()
	label.text = "Keys A-K play notes - mouse X: filter, mouse Y: pitch bend"
	label.position = Vector2(16, 16)
	add_child(label)


func _unhandled_input(event: InputEvent) -> void:
	if event is InputEventKey and not event.echo:
		var index := KEYS.find(event.keycode)
		if index >= 0:
			_stop_arpeggio()
			_idle = 0.0
			if event.pressed:
				_synth.note_on(SCALE[index], 100)
			else:
				_synth.note_off(SCALE[index])
	elif event is InputEventMouseMotion:
		var size := get_viewport().get_visible_rect().size
		_synth.control_change(1, int(clampf(event.position.x / size.x, 0.0, 1.0) * 127.0))
		_synth.pitch_bend(1.0 - 2.0 * clampf(event.position.y / size.y, 0.0, 1.0))


func _process(delta: float) -> void:
	_idle += delta
	if _idle < 3.0:
		return
	_step_timer -= delta
	if _step_timer > 0.0:
		return
	_step_timer = 0.25
	_stop_arpeggio()
	_arp_note = ARPEGGIO[_step % ARPEGGIO.size()]
	_synth.note_on(_arp_note, 90)
	_step += 1


func _stop_arpeggio() -> void:
	if _arp_note >= 0:
		_synth.note_off(_arp_note)
		_arp_note = -1
