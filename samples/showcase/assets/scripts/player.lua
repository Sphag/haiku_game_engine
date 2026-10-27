-- The hero: reads input, moves the entity, steers the animation graph, and tells the HUD when it jumps. A host that does not bind
-- input or animation (a headless run) leaves those calls failing, so each is wrapped and the script keeps moving.
local speed = 0
local walk_speed = 3.0

local function try(fn, ...)
  local ok, value = pcall(fn, ...)
  if ok then return value end
  return nil
end

function create(entity)
  events.on('hud.reset', function() speed = 0 end)
end

function fixed_update(entity, dt)
  local move = try(input.value, 'move_forward') or 0
  speed = move * walk_speed
  local t = transform.get(entity)
  transform.set(entity, { position = { t.position[1], t.position[2], t.position[3] - speed * dt } })
end

function update(entity, dt)
  try(animation.set_float, 'hero', 'speed', math.abs(speed))
  if try(input.pressed, 'jump') then
    try(animation.trigger, 'hero', 'jump')
    events.emit('hero.jump', entity)
  end
end

function destroy(entity)
  events.emit('hero.gone', entity)
end
