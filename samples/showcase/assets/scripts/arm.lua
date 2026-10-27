-- Switches the arm's animation graph between its two clips every three seconds, through the animation module.
local elapsed = 0

function update(entity, dt)
  elapsed = elapsed + dt
  local excite = (math.floor(elapsed / 3) % 2 == 0) and 0 or 1
  animation.set_float('arm', 'excite', excite)
end
