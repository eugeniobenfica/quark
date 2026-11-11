-- The same work in Lua 5.4, written the way the Scopes plugin is.
local floor = math.floor
local function luma(r, g, b) return floor(0.2126 * r + 0.7152 * g + 0.0722 * b + 0.5) end
local n = 384 * 216
local rgb = {}
for i = 0, n * 3 - 1 do rgb[i] = ((i * 2654435761 + 12345) // 7) % 256 end
local start = os.clock()
local checksum = 0
for frame = 0, 199 do
  local red, green, blue, light = {}, {}, {}, {}
  for i = 0, 255 do red[i] = 0; green[i] = 0; blue[i] = 0; light[i] = 0 end
  for p = 0, n - 1 do local v = rgb[p * 3]; red[v] = red[v] + 1 end
  for p = 0, n - 1 do local v = rgb[p * 3 + 1]; green[v] = green[v] + 1 end
  for p = 0, n - 1 do local v = rgb[p * 3 + 2]; blue[v] = blue[v] + 1 end
  for p = 0, n - 1 do local v = luma(rgb[p * 3], rgb[p * 3 + 1], rgb[p * 3 + 2]); light[v] = light[v] + 1 end
  checksum = checksum + red[frame % 256] + green[7] + blue[200] + light[frame % 256]
end
print("checksum", checksum)
print("ms", (os.clock() - start) * 1000)
