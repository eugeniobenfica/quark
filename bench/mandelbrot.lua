local function inside(cx, cy)
  local x, y = 0.0, 0.0
  for i = 1, 100 do
    local xx, yy = x * x, y * y
    if xx + yy > 4.0 then return false end
    y = 2.0 * x * y + cy
    x = xx - yy + cx
  end
  return true
end
local start = os.clock()
local size, count = 1000, 0
for k = 0, size * size - 1 do
  if inside((k % size) * 3.0 / size - 2.0, (k // size) * 3.0 / size - 1.5) then count = count + 1 end
end
print("inside", count)
print("ms", (os.clock() - start) * 1000)
