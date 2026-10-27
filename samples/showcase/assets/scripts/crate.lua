-- Spins a crate slowly and records how long it has been running. A script is attached through the engine.script component.
local elapsed = 0

function create(entity)
  elapsed = 0
end

function update(entity, dt)
  elapsed = elapsed + dt
  local t = transform.get(entity)
  local angle = elapsed * 0.5
  transform.set(entity, { rotation = { math.cos(angle / 2), 0, math.sin(angle / 2), 0 } })
end

function export_state()
  return { elapsed = elapsed }
end

function import_state(state)
  elapsed = state.elapsed or 0
end
