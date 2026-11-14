-- The same work in Lua 5.4.
local floor = math.floor
local function blend(a, b, c) return floor(0.2126 * a + 0.7152 * b + 0.0722 * c + 0.5) end
local n = 384 * 216
local rows = {}
for i = 0, n * 3 - 1 do rows[i] = ((i * 2654435761 + 12345) // 7) % 256 end
local start = os.clock()
local checksum = 0
for pass = 0, 199 do
  local first, second, third, blended = {}, {}, {}, {}
  for i = 0, 255 do first[i] = 0; second[i] = 0; third[i] = 0; blended[i] = 0 end
  for p = 0, n - 1 do local v = rows[p * 3]; first[v] = first[v] + 1 end
  for p = 0, n - 1 do local v = rows[p * 3 + 1]; second[v] = second[v] + 1 end
  for p = 0, n - 1 do local v = rows[p * 3 + 2]; third[v] = third[v] + 1 end
  for p = 0, n - 1 do local v = blend(rows[p * 3], rows[p * 3 + 1], rows[p * 3 + 2]); blended[v] = blended[v] + 1 end
  checksum = checksum + first[pass % 256] + second[7] + third[200] + blended[pass % 256]
end
print("checksum", checksum)
print("ms", (os.clock() - start) * 1000)
