-- gmod-mcp: standalone JSON encoder/decoder (no dependency on GMod functions).
__mcpagent = __mcpagent or {}
local M = {}
__mcpagent.json = M

local string_byte, string_char, string_format, string_sub, string_find = string.byte, string.char, string.format, string.sub, string.find
local table_concat, math_floor, math_huge, pairs, type, tostring, setmetatable, getmetatable, next =
	table.concat, math.floor, math.huge, pairs, type, tostring, setmetatable, getmetatable, next

M.null = setmetatable({}, { __tostring = function() return "null" end, __jsonnull = true })
local ARRAY_MT = { __jsontype = "array" }
local OBJECT_MT = { __jsontype = "object" }
function M.array(t) return setmetatable(t or {}, ARRAY_MT) end
function M.object(t) return setmetatable(t or {}, OBJECT_MT) end

local escapes = {
	['"'] = '\\"', ["\\"] = "\\\\", ["\b"] = "\\b", ["\f"] = "\\f", ["\n"] = "\\n", ["\r"] = "\\r", ["\t"] = "\\t",
}

-- Encodes a string as JSON, validating UTF-8; invalid bytes become \u00XX (Latin-1).
local function encode_string(s)
	if not string_find(s, '[%z\1-\31"\\\128-\255]') then return '"' .. s .. '"' end
	local out, n, i, len = {}, 0, 1, #s
	while i <= len do
		local c = string_byte(s, i)
		local piece, step
		if c < 0x80 then
			local ch = string_char(c)
			piece = escapes[ch] or (c < 0x20 and string_format("\\u%04x", c)) or ch
			step = 1
		else
			local need = (c >= 0xC2 and c <= 0xDF) and 1 or (c >= 0xE0 and c <= 0xEF) and 2 or (c >= 0xF0 and c <= 0xF4) and 3 or 0
			-- Strict UTF-8 (RFC 3629), as the module's JSON parser checks it: the second byte's range excludes overlong
			-- forms (E0, F0), UTF-16 surrogates (ED) and code points above U+10FFFF (F4).
			local lo2 = c == 0xE0 and 0xA0 or c == 0xF0 and 0x90 or 0x80
			local hi2 = c == 0xED and 0x9F or c == 0xF4 and 0x8F or 0xBF
			local ok = need > 0 and i + need <= len
			if ok then
				for k = 1, need do
					local cc = string_byte(s, i + k)
					if cc < (k == 1 and lo2 or 0x80) or cc > (k == 1 and hi2 or 0xBF) then ok = false break end
				end
			end
			if ok then
				piece = string_sub(s, i, i + need)
				step = need + 1
			else
				piece = string_format("\\u%04x", c)
				step = 1
			end
		end
		n = n + 1
		out[n] = piece
		i = i + step
	end
	return '"' .. table_concat(out) .. '"'
end
M.encode_string = encode_string

local function encode_number(v)
	if v ~= v or v == math_huge or v == -math_huge then return "null" end
	if v == math_floor(v) and v > -9007199254740992 and v < 9007199254740992 then return string_format("%d", v) end
	return string_format("%.17g", v)
end

local function is_array(t)
	local mt = getmetatable(t)
	if mt == ARRAY_MT then return true end
	if mt == OBJECT_MT then return false end
	local n = 0
	for k in pairs(t) do
		if type(k) ~= "number" or k < 1 or k ~= math_floor(k) then return false end
		n = n + 1
	end
	if n == 0 then return false end
	for i = 1, n do if t[i] == nil then return false end end
	return true, n
end

local encode_value
encode_value = function(v, buf, n, seen)
	local tv = type(v)
	if v == nil or v == M.null then
		n = n + 1; buf[n] = "null"
	elseif tv == "boolean" then
		n = n + 1; buf[n] = v and "true" or "false"
	elseif tv == "number" then
		n = n + 1; buf[n] = encode_number(v)
	elseif tv == "string" then
		n = n + 1; buf[n] = encode_string(v)
	elseif tv == "table" then
		if seen[v] then
			n = n + 1; buf[n] = '{"__type":"cycle"}'
			return n
		end
		seen[v] = true
		local arr, cnt = is_array(v)
		if arr then
			cnt = cnt or #v
			n = n + 1; buf[n] = "["
			for i = 1, cnt do
				if i > 1 then n = n + 1; buf[n] = "," end
				n = encode_value(v[i], buf, n, seen)
			end
			n = n + 1; buf[n] = "]"
		else
			n = n + 1; buf[n] = "{"
			local first = true
			for k, val in pairs(v) do
				if not first then n = n + 1; buf[n] = "," end
				first = false
				n = n + 1; buf[n] = encode_string(type(k) == "string" and k or tostring(k))
				n = n + 1; buf[n] = ":"
				n = encode_value(val, buf, n, seen)
			end
			n = n + 1; buf[n] = "}"
		end
		seen[v] = nil
	else
		n = n + 1; buf[n] = encode_string(tostring(v))
	end
	return n
end

function M.encode(v)
	local buf = {}
	encode_value(v, buf, 0, {})
	return table_concat(buf)
end

-- Decoder.
local decode_value
local function skip(s, i)
	local _, e = string_find(s, "^[ \t\r\n]*", i)
	return e + 1
end

local function decode_error(s, i, msg) error(string_format("invalid JSON at position %d: %s", i, msg), 0) end

local function utf8_char(cp)
	if cp < 0x80 then return string_char(cp) end
	if cp < 0x800 then return string_char(0xC0 + math_floor(cp / 0x40), 0x80 + cp % 0x40) end
	if cp < 0x10000 then
		return string_char(0xE0 + math_floor(cp / 0x1000), 0x80 + math_floor(cp / 0x40) % 0x40, 0x80 + cp % 0x40)
	end
	return string_char(0xF0 + math_floor(cp / 0x40000), 0x80 + math_floor(cp / 0x1000) % 0x40,
		0x80 + math_floor(cp / 0x40) % 0x40, 0x80 + cp % 0x40)
end

local function decode_string(s, i)
	-- s[i] == '"'
	local out, n, j = {}, 0, i + 1
	while true do
		local a, b = string_find(s, '["\\]', j)
		if not a then decode_error(s, i, "unterminated string") end
		if a > j then n = n + 1; out[n] = string_sub(s, j, a - 1) end
		if string_byte(s, a) == 34 then return table_concat(out), a + 1 end
		local e = string_sub(s, a + 1, a + 1)
		if e == "u" then
			local hex = string_sub(s, a + 2, a + 5)
			local cp = tonumber(hex, 16)
			if not cp then decode_error(s, a, "invalid \\u escape") end
			local nexti = a + 6
			if cp >= 0xD800 and cp <= 0xDBFF and string_sub(s, nexti, nexti + 1) == "\\u" then
				local lo = tonumber(string_sub(s, nexti + 2, nexti + 5), 16)
				if lo and lo >= 0xDC00 and lo <= 0xDFFF then
					cp = 0x10000 + (cp - 0xD800) * 0x400 + (lo - 0xDC00)
					nexti = nexti + 6
				end
			end
			n = n + 1; out[n] = utf8_char(cp)
			j = nexti
		else
			local map = { ['"'] = '"', ["\\"] = "\\", ["/"] = "/", b = "\b", f = "\f", n = "\n", r = "\r", t = "\t" }
			local c = map[e]
			if not c then decode_error(s, a, "invalid escape") end
			n = n + 1; out[n] = c
			j = a + 2
		end
	end
end

decode_value = function(s, i)
	i = skip(s, i)
	local c = string_sub(s, i, i)
	if c == "{" then
		local obj = {}
		i = skip(s, i + 1)
		if string_sub(s, i, i) == "}" then return M.object(obj), i + 1 end
		while true do
			i = skip(s, i)
			if string_sub(s, i, i) ~= '"' then decode_error(s, i, "expected key") end
			local k
			k, i = decode_string(s, i)
			i = skip(s, i)
			if string_sub(s, i, i) ~= ":" then decode_error(s, i, "expected ':'") end
			local v
			v, i = decode_value(s, i + 1)
			if v ~= M.null then obj[k] = v end
			i = skip(s, i)
			local d = string_sub(s, i, i)
			if d == "}" then return obj, i + 1 end
			if d ~= "," then decode_error(s, i, "expected ',' or '}'") end
			i = i + 1
		end
	elseif c == "[" then
		local arr, n = M.array({}), 0
		i = skip(s, i + 1)
		if string_sub(s, i, i) == "]" then return arr, i + 1 end
		while true do
			local v
			v, i = decode_value(s, i)
			n = n + 1
			arr[n] = v
			i = skip(s, i)
			local d = string_sub(s, i, i)
			if d == "]" then return arr, i + 1 end
			if d ~= "," then decode_error(s, i, "expected ',' or ']'") end
			i = i + 1
		end
	elseif c == '"' then
		return decode_string(s, i)
	elseif string_sub(s, i, i + 3) == "true" then
		return true, i + 4
	elseif string_sub(s, i, i + 4) == "false" then
		return false, i + 5
	elseif string_sub(s, i, i + 3) == "null" then
		return M.null, i + 4
	else
		local a, b = string_find(s, "^-?%d+%.?%d*[eE]?[-+]?%d*", i)
		if not a then decode_error(s, i, "unexpected value") end
		return tonumber(string_sub(s, a, b)), b + 1
	end
end

function M.decode(s)
	local v, i = decode_value(s, 1)
	i = skip(s, i)
	if i <= #s then decode_error(s, i, "trailing data") end
	return v
end
