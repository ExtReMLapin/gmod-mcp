-- gmod-mcp: sound, performance, API introspection, HTTP (wiki).
local A = __mcpagent
local J = A.json
local H = A.handlers

-- Sound ---------------------------------------------------------------------------------------------
if A.realm ~= "menu" then
	function H.sound_emit(args)
		local name = args.name
		local exists = (sound.GetProperties and sound.GetProperties(name) ~= nil) or file.Exists("sound/" .. name, "GAME")
		if not exists then A.fail("not_found", "sound not found (neither a soundscript nor a file sound/" .. tostring(name) .. ")") end
		local level, pitch, vol, dsp = args.level or 75, args.pitch or 100, args.volume or 1, args.dsp or 0
		if args.ent then
			local e = A.get_ent(args.ent)
			e:EmitSound(name, level, pitch, vol, CHAN_AUTO, 0, dsp)
			return { emitted = name, ent = args.ent, level = level, pitch = pitch, volume = vol, dsp = dsp }
		end
		local pos = A.to_vector(args.pos, "pos")
		if not pos then A.fail("invalid_params", "pos or ent is required") end
		EmitSound(name, pos, 0, CHAN_AUTO, vol, level, 0, pitch, dsp)
		return { emitted = name, pos = A.vec_out(pos), level = level, pitch = pitch, volume = vol, dsp = dsp }
	end

	-- EntityEmitSound trace with normalized fields.
	function H.sound_trace_start(args)
		local id = A.trace_start("EntityEmitSound", args.max_calls or 100, 0, function(c, data)
			c.args = nil
			if type(data) ~= "table" then return end
			c.sound = {
				name = data.SoundName, original_name = data.OriginalSoundName, level = data.SoundLevel, pitch = data.Pitch,
				volume = data.Volume, dsp = data.DSP, channel = data.Channel, flags = data.Flags, sound_time = data.SoundTime,
				pos = data.Pos and A.vec_out(data.Pos) or J.null,
				entity = IsValid(data.Entity) and A.ent_summary(data.Entity) or (data.Entity == game.GetWorld() and { id = 0, class = "worldspawn" } or J.null),
			}
		end)
		return { id = id }
	end
end

-- Performance ---------------------------------------------------------------------------------------
function H.perf(args)
	local o = { realm = A.realm, lua_memory_kb = collectgarbage("count") }
	if A.realm ~= "menu" then
		o.entity_count = ents.GetCount()
		o.frametime = FrameTime()
		o.tick_interval = engine.TickInterval()
		if A.realm == "client" then o.real_frametime = RealFrameTime() end
	end
	return o
end

-- Introspection of the API actually present ---------------------------------------------------------
local META_CANDIDATES = {
	"Entity", "Player", "Weapon", "NPC", "Vehicle", "NextBot", "PhysObj", "Vector", "Angle", "VMatrix", "IMaterial", "ITexture",
	"CEffectData", "CMoveData", "CUserCmd", "ConVar", "Color", "IMesh", "CLuaParticle", "CLuaEmitter", "Panel", "bf_read",
	"CTakeDamageInfo", "CSoundPatch", "File", "IVideoWriter", "ProjectedTexture", "PathFollower", "CNavArea", "CNavLadder",
	"Stack", "Tool", "pixelvis_handle_t", "IGModAudioChannel", "IRestore", "ISave", "CNewParticleEffect", "CLuaLocomotion",
	"MarkupObject", "SurfaceInfo", "dlight_t", "CRecipientFilter", "IVideoWriter", "Schedule", "Task", "Pixelvis",
}

local function build_index(extra_meta)
	local idx = {}
	local seen = {}
	local function add(name, kind) if not seen[name] then seen[name] = true idx[#idx + 1] = { name = name, kind = kind } end end
	for k, v in pairs(_G) do
		if type(k) == "string" then
			if type(v) == "function" then
				add(k, "global")
			elseif type(v) == "table" and k ~= "_G" and k ~= "__mcpagent" and k ~= "mcpagent" and not string.find(k, "^__") then
				for k2, v2 in pairs(v) do
					if type(k2) == "string" and type(v2) == "function" then add(k .. "." .. k2, "library") end
				end
			end
		end
	end
	-- Metatables: not enumerable from the registry in this build, so probe names with FindMetaTable
	-- (built-in list + class prefixes from the cached wiki page list, passed by the DLL).
	local names = {}
	for _, n in ipairs(META_CANDIDATES) do names[n] = true end
	for _, n in ipairs(extra_meta or {}) do names[n] = true end
	for name in pairs(names) do
		local mt = FindMetaTable and FindMetaTable(name)
		if type(mt) == "table" then
			local mname = rawget(mt, "MetaName") or name
			local src = rawget(mt, "__index")
			for _, t in ipairs({ mt, type(src) == "table" and src or nil }) do
				for k, f in pairs(t) do
					if type(k) == "string" and type(f) == "function" and not string.find(k, "^__") then add(mname .. ":" .. k, "method") end
				end
			end
		end
	end
	table.sort(idx, function(a, b) return a.name < b.name end)
	return idx
end

function H.api_search(args)
	local extra = args.meta_names or {}
	if not A.api_index or (A.api_index_extra or 0) ~= #extra then
		A.api_index = build_index(extra)
		A.api_index_extra = #extra
	end
	local pat = string.lower(args.pattern or "")
	local glob = string.find(pat, "*", 1, true)
	local lp = glob and ("^" .. string.gsub(string.gsub(pat, "[%^%$%(%)%%%.%[%]%+%-%?]", "%%%0"), "%*", ".*") .. "$") or nil
	local out = J.array({})
	for _, e in ipairs(A.api_index) do
		local n = string.lower(e.name)
		if (lp and string.find(n, lp)) or (not lp and string.find(n, pat, 1, true)) then
			out[#out + 1] = e
			if #out >= (args.limit or 500) then break end
		end
	end
	return { realm = A.realm, matches = out, index_size = #A.api_index }
end

-- Outgoing HTTP: only GETs to the wiki (hard-coded allowlist) -----------------------------------------
local WIKI_PREFIX = "https://wiki.facepunch.com/gmod/"
A.http = A.http or { next_id = 0, req = {} }
local HT = A.http

function H.http_get(args)
	local url = tostring(args.url or "")
	if string.sub(url, 1, #WIKI_PREFIX) ~= WIKI_PREFIX then A.fail("forbidden", "only " .. WIKI_PREFIX .. "* URLs are allowed") end
	if not HTTP then A.fail("unsupported", "HTTP function unavailable in this realm") end
	HT.next_id = HT.next_id + 1
	local id = HT.next_id
	HT.req[id] = { done = false }
	local ok = HTTP({
		url = url, method = "GET",
		success = function(code, body, headers) HT.req[id] = { done = true, code = code, body = body } end,
		failed = function(reason) HT.req[id] = { done = true, error = tostring(reason) } end,
	})
	if ok == false then HT.req[id] = { done = true, error = "HTTP request rejected by the engine" } end
	return { id = id }
end

function H.http_poll(args)
	local r = HT.req[args.id]
	if not r then A.fail("not_found", "unknown request") end
	if not r.done then return { done = false } end
	HT.req[args.id] = nil
	if r.error then return { done = true, error = r.error } end
	A.blob = r.body or ""
	return { done = true, code = r.code, bytes = #A.blob }
end
