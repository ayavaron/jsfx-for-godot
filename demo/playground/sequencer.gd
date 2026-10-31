extends Node

# Plays a parsed MIDI file (midi_file.gd) in real time and emits its notes.
# Events go out on the frame they fall due, so they reach the synths at the
# next audio block: fine for a demo track, a few milliseconds of jitter.

const MidiFile := preload("res://playground/midi_file.gd")

signal note_on(channel: int, note: int, velocity: int)
signal note_off(channel: int, note: int)
signal finished

var midi := MidiFile.new()
var position := 0.0
var playing := false
var _next := 0
# Notes this sequencer has started and not yet stopped: [channel, note].
var _sounding := {}


func load_file(path: String) -> bool:
	return midi.load_file(path)


func play(from_seconds := 0.0) -> void:
	position = from_seconds
	_next = 0
	while _next < midi.events.size() and midi.events[_next][0] < from_seconds:
		_next += 1
	playing = true


func stop() -> void:
	playing = false
	for key in _sounding.keys():
		note_off.emit(key[0], key[1])
	_sounding.clear()


func _process(delta: float) -> void:
	if not playing:
		return
	position += delta
	var events := midi.events
	while _next < events.size() and events[_next][0] <= position:
		var event: Array = events[_next]
		_next += 1
		var channel: int = event[1] & 0x0F
		var key := [channel, event[2]]
		if event[1] & 0xF0 == 0x90:
			# Doubled parts restart a note that is already sounding.
			_sounding[key] = _sounding.get(key, 0) + 1
			note_on.emit(channel, event[2], event[3])
		elif _sounding.has(key):
			_sounding[key] -= 1
			if _sounding[key] <= 0:
				_sounding.erase(key)
			note_off.emit(channel, event[2])
	if _next >= events.size():
		stop()
		finished.emit()
