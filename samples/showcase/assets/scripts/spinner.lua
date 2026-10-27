-- Turns a kinematic platform around the vertical axis at a fixed rate in the fixed step.
local angle = 0

function fixed_update(entity, dt)
  angle = angle + dt
  transform.set(entity, { rotation = { math.cos(angle / 2), 0, math.sin(angle / 2), 0 } })
end
