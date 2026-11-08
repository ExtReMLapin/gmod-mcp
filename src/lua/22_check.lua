-- gmod-mcp: lua_check. Compiles code with the game's own parser (GLua syntax: !=, &&, ||, //, continue...) without
-- running it, then reads the bytecode (string.dump) to list the globals each chunk reads and writes.
local A = __mcpagent
local J = A.json
local H = A.handlers

-- Splits a compiler message "chunk:LINE: text" into its line and text.
function A.compile_error_line(msg)
	msg = tostring(msg)
	local line, text = msg:match("^.-:(%d+): (.*)$")
	if line then return tonumber(line), text end
	return nil, msg
end

-- LuaJIT 2.x bytecode dump reader (format of lj_bcdump.h), just enough to find GGET/GSET and their lines.
local function uleb(s, p)
	local v, mul = 0, 1
	while true do
		local b = s:byte(p)
		if not b then error("truncated bytecode", 0) end
		p = p + 1
		v = v + (b % 128) * mul
		if b < 128 then return v, p end
		mul = mul * 128
	end
end

local function skip_uleb(s, p)
	while true do
		local b = s:byte(p)
		if not b then error("truncated bytecode", 0) end
		p = p + 1
		if b < 128 then return p end
	end
end

local function skip_ktabk(s, p)
	local tp
	tp, p = uleb(s, p)
	if tp >= 5 then return p + tp - 5 end
	if tp == 3 then return skip_uleb(s, p) end
	if tp == 4 then return skip_uleb(s, skip_uleb(s, p)) end
	return p
end

-- Calls visit(op, name, line) for every instruction whose D operand is a string constant (op in ops).
local function scan_dump(s, ops, visit)
	if s:sub(1, 3) ~= "\27LJ" then error("not LuaJIT bytecode", 0) end
	local p = 5
	local flags
	flags, p = uleb(s, p)
	local be = flags % 2 == 1
	local strip = math.floor(flags / 2) % 2 == 1
	if not strip then
		local n
		n, p = uleb(s, p)
		p = p + n
	end
	while true do
		local len
		len, p = uleb(s, p)
		if len == 0 then break end
		local stop = p + len
		local sizeuv = s:byte(p + 3)
		p = p + 4
		local nkgc, nkn, nbc, ndbg, firstline, numline = 0, 0, 0, 0, 0, 0
		nkgc, p = uleb(s, p)
		nkn, p = uleb(s, p)
		nbc, p = uleb(s, p)
		if not strip then
			ndbg, p = uleb(s, p)
			if ndbg > 0 then
				firstline, p = uleb(s, p)
				numline, p = uleb(s, p)
			end
		end
		local bcpos = p
		p = p + nbc * 4 + sizeuv * 2
		local kgc = {}
		for i = 1, nkgc do
			local tp
			tp, p = uleb(s, p)
			if tp >= 5 then
				kgc[i] = s:sub(p, p + tp - 6)
				p = p + tp - 5
			elseif tp == 1 then
				local na, nh
				na, p = uleb(s, p)
				nh, p = uleb(s, p)
				for _ = 1, na + 2 * nh do p = skip_ktabk(s, p) end
			elseif tp == 2 or tp == 3 then
				p = skip_uleb(s, skip_uleb(s, p))
			elseif tp == 4 then
				for _ = 1, 4 do p = skip_uleb(s, p) end
			end
		end
		for _ = 1, nkn do
			local isnum = s:byte(p) % 2 == 1
			p = skip_uleb(s, p)
			if isnum then p = skip_uleb(s, p) end
		end
		local w = numline < 256 and 1 or (numline < 65536 and 2 or 4)
		for i = 0, nbc - 1 do
			local q = bcpos + i * 4
			local b0, b1, b2, b3 = s:byte(q, q + 3)
			local op, d
			if be then op, d = b3, b0 * 256 + b1 else op, d = b0, b3 * 256 + b2 end
			if ops[op] then
				local line
				if ndbg > 0 then
					local lq, li = p + i * w, 0
					for k = w - 1, 0, -1 do
						li = li * 256 + s:byte(be and (lq + w - 1 - k) or (lq + k))
					end
					line = firstline + li
				end
				local name = kgc[nkgc - d]
				if name then visit(ops[op], name, line) end
			end
		end
		p = stop
	end
end

-- Opcode numbers differ between LuaJIT builds: read them from a probe chunk "a = b" (GGET, then GSET).
local OPS
local function opcodes()
	if OPS then return OPS end
	local f = A.compile("a = b", "=mcpagent:probe")
	local s, seen = string.dump(f), {}
	scan_dump(s, setmetatable({}, { __index = function(_, op) seen[#seen + 1] = op return nil end }), function() end)
	if #seen < 2 or seen[1] == seen[2] then error("cannot identify GGET/GSET in this LuaJIT build", 0) end
	OPS = { [seen[1]] = "read", [seen[2]] = "write" }
	return OPS
end

-- Globals set by the engine while it loads a scripted entity, weapon, gamemode, effect or tool.
local INJECTED = { ENT = true, SWEP = true, GM = true, EFFECT = true, TOOL = true }

local function sorted(list, cap)
	table.sort(list, function(a, b)
		if a.file ~= b.file then return a.file < b.file end
		if (a.line or 0) ~= (b.line or 0) then return (a.line or 0) < (b.line or 0) end
		return a.name < b.name
	end)
	local out, truncated = J.array({}), false
	for i, v in ipairs(list) do
		if i > cap then truncated = true break end
		out[i] = v
	end
	return out, truncated
end

function H.lua_check(args)
	local items = args.items
	if type(items) ~= "table" then A.fail("invalid_params", "items: expected an array") end
	local want_globals = args.globals ~= false
	local cap = args.limit or 200
	local errors, reads, writes, written, notes = {}, {}, {}, {}, J.array({})
	local ops
	if want_globals then
		local ok, e = pcall(opcodes)
		if ok then ops = e else notes[#notes + 1] = "globals not checked: " .. tostring(e) end
	end
	for _, it in ipairs(items) do
		local fn, cerr = A.compile(it.code, it.chunk)
		if not fn then
			local line, text = A.compile_error_line(cerr)
			errors[#errors + 1] = { file = it.file, line = line, message = text }
		elseif ops then
			local ok, e = pcall(scan_dump, string.dump(fn), ops, function(kind, name, line)
				local rec = { file = it.file, line = line, name = name }
				if kind == "write" then
					writes[#writes + 1] = rec
					written[name] = true
				else
					reads[#reads + 1] = rec
				end
			end)
			if not ok then notes[#notes + 1] = it.file .. ": globals not checked: " .. tostring(e) end
		end
	end
	local unknown = {}
	for _, r in ipairs(reads) do
		if not written[r.name] and not INJECTED[r.name] and rawget(_G, r.name) == nil then unknown[#unknown + 1] = r end
	end
	local out = { ok = #errors == 0, realm = A.realm, checked = #items }
	out.errors = sorted(errors, cap)
	if ops then
		local tr1, tr2
		out.unknown_globals, tr1 = sorted(unknown, cap)
		out.globals_written, tr2 = sorted(writes, cap)
		if tr1 or tr2 then out.truncated = true end
	end
	if #notes > 0 then out.notes = notes end
	return out
end
