-- Builds the heads-up display callbacks. The host binds a button to on_play with script.ui_action; the label text is plain data.
presses = 0
volume = 0.8

function on_play()
  presses = presses + 1
  events.emit('hud.play', presses)
end

function on_volume(value)
  volume = value
  audio.set_bus_volume('master', value)
end
