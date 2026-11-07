-- gmod-mcp: world (entities, players, bots, traces, surface traversal).
local A = __mcpagent
local J = A.json
local H = A.handlers
if A.realm == "menu" then return end

local vec_out, ang_out = A.vec_out, A.ang_out

local function glob_to_pattern(g)
	return "^" .. string.gsub(string.gsub(g, "[%^%$%(%)%%%.%[%]%+%-%?]", "%%%0"), "%*", ".*") .. "$"
end

local function ent_summary(e)
	local o = { id = e:EntIndex(), class = e:GetClass(), pos = vec_out(e:GetPos()), ang = ang_out(e:GetAngles()) }
	local m = e:GetModel()
	if m and m ~= "" then o.model = m end
	if e.GetName and e:GetName() ~= "" then o.name = e:GetName() end
	if e:IsPlayer() then o.nick = e:Nick() end
	return o
end
A.ent_summary = ent_summary

local function get_ent(id)
	local e = Entity(tonumber(id) or -1)
	if id == 0 then return game.GetWorld() end
	if not IsValid(e) then A.fail("not_found", "invalid entity: " .. tostring(id)) end
	return e
end
A.get_ent = get_ent

function H.ents_find(args)
	local limit = args.limit or 100
	local cls = args.class and string.lower(args.class)
	local cpat = cls and glob_to_pattern(cls)
	local mpat = args.model and glob_to_pattern(string.lower(args.model))
	local near = args.near and A.to_vector(args.near, "near")
	local radius = args.radius or 512
	local list = near and ents.FindInSphere(near, radius) or ents.GetAll()
	local out, total = J.array({}), 0
	for _, e in ipairs(list) do
		if IsValid(e) then
			local ok = true
			if cpat and not string.find(string.lower(e:GetClass()), cpat) then ok = false end
			if ok and mpat and not string.find(string.lower(e:GetModel() or ""), mpat) then ok = false end
			if ok then
				total = total + 1
				if #out < limit then
					local s = ent_summary(e)
					if near then s.distance = e:GetPos():Distance(near) end
					out[#out + 1] = s
				end
			end
		end
	end
	if near then table.sort(out, function(a, b) return a.distance < b.distance end) end
	return { entities = out, count = total, truncated = total > #out }
end

function H.ent_info(args)
	local e = get_ent(args.id)
	local o = ent_summary(e)
	o.valid = IsValid(e)
	o.velocity = vec_out(e:GetVelocity())
	o.health = e:Health()
	o.max_health = e:GetMaxHealth()
	o.solid = e:GetSolid()
	o.movetype = e:GetMoveType()
	o.collision_group = e:GetCollisionGroup()
	local mins, maxs = e:OBBMins(), e:OBBMaxs()
	o.obb = { mins = vec_out(mins), maxs = vec_out(maxs) }
	o.parent = IsValid(e:GetParent()) and e:GetParent():EntIndex() or J.null
	o.owner = IsValid(e:GetOwner()) and e:GetOwner():EntIndex() or J.null
	o.nw_vars = A.serialize(e:GetNWVarTable() or {}, { depth = 2, limit = 200 })
	o.table = A.serialize(e:GetTable() or {}, { depth = args.depth or 1, limit = args.limit or 100 })
	if SERVER then
		o.keyvalues = A.serialize(e:GetKeyValues() or {}, { depth = 1, limit = 300 })
		local cls = e:GetClass()
		if string.find(cls, "door", 1, true) then
			local st = e:GetInternalVariable("m_eDoorState")
			local tog = e:GetInternalVariable("m_toggle_state")
			local names = { [0] = "closed", [1] = "opening", [2] = "open", [3] = "closing", [4] = "ajar" }
			o.door = { m_eDoorState = st, m_toggle_state = tog,
				state = st and names[st] or (tog == 0 and "open" or tog == 1 and "closed" or tog == 2 and "opening" or tog == 3 and "closing" or J.null) }
		end
	end
	return o
end

function H.ent_spawn(args)
	if not SERVER then A.fail("wrong_realm", "ent_spawn: requires the server realm") end
	local e = ents.Create(args.class)
	if not IsValid(e) then A.fail("invalid_class", "unknown entity class: " .. tostring(args.class)) end
	if args.model then e:SetModel(args.model) end
	if args.pos then e:SetPos(A.to_vector(args.pos, "pos")) end
	if args.ang then e:SetAngles(A.to_angle(args.ang, "ang")) end
	for k, v in pairs(args.keyvalues or {}) do e:SetKeyValue(k, tostring(v)) end
	e:Spawn()
	e:Activate()
	if args.freeze then
		local phys = e:GetPhysicsObject()
		if IsValid(phys) then phys:EnableMotion(false) end
	end
	return ent_summary(e)
end

function H.ent_remove(args)
	if not SERVER then A.fail("wrong_realm", "ent_remove: requires the server realm") end
	local e = get_ent(args.id)
	if e:IsPlayer() or e == game.GetWorld() then A.fail("forbidden", "cannot remove a player or the world") end
	e:Remove()
	return { removed = args.id }
end

function H.ent_fire(args)
	if not SERVER then A.fail("wrong_realm", "ent_fire: requires the server realm") end
	local e = get_ent(args.id)
	e:Fire(args.input, args.param or "", args.delay or 0)
	return { fired = args.input, id = args.id, param = args.param or "", delay = args.delay or 0 }
end

function H.players_list(args)
	local out = J.array({})
	for _, p in ipairs(player.GetAll()) do
		local o = ent_summary(p)
		o.userid = p:UserID()
		o.steamid = p:SteamID()
		o.bot = p:IsBot()
		o.alive = p:Alive()
		o.health = p:Health()
		o.eye_pos = vec_out(p:EyePos())
		o.eye_ang = ang_out(p:EyeAngles())
		o.noclip = p:GetMoveType() == MOVETYPE_NOCLIP
		if SERVER then o.god = p:HasGodMode() end
		out[#out + 1] = o
	end
	return { players = out, count = #out, maxplayers = game.MaxPlayers() }
end

function H.player_set(args)
	if not SERVER then A.fail("wrong_realm", "player_set: requires the server realm") end
	local p = args.id and Entity(args.id) or player.GetAll()[1]
	if not IsValid(p) or not p:IsPlayer() then A.fail("not_found", "player not found: " .. tostring(args.id)) end
	if args.pos then p:SetPos(A.to_vector(args.pos, "pos")) p:SetVelocity(-p:GetVelocity()) end
	if args.ang then p:SetEyeAngles(A.to_angle(args.ang, "ang")) end
	if args.noclip ~= nil then p:SetMoveType(args.noclip and MOVETYPE_NOCLIP or MOVETYPE_WALK) end
	if args.god ~= nil then if args.god then p:GodEnable() else p:GodDisable() end end
	local o = ent_summary(p)
	o.eye_ang = ang_out(p:EyeAngles())
	o.noclip = p:GetMoveType() == MOVETYPE_NOCLIP
	o.god = p:HasGodMode()
	return o
end

function H.bots_add(args)
	if not SERVER then A.fail("wrong_realm", "bots_add: requires the server realm") end
	local n = args.n or 1
	local free = game.MaxPlayers() - #player.GetAll()
	if free < n then
		A.fail("no_slots", string.format("not enough slots: maxplayers=%d, players=%d, requested=%d (load the map with a higher maxplayers)",
			game.MaxPlayers(), #player.GetAll(), n))
	end
	local out = J.array({})
	for i = 1, n do
		local b = player.CreateNextBot((args.name or "mcpbot") .. "_" .. (#player.GetBots() + 1))
		if IsValid(b) then out[#out + 1] = ent_summary(b) end
	end
	return { added = out, count = #out }
end

-- Traces.
local MASKS = {
	MASK_SOLID = MASK_SOLID, MASK_SHOT = MASK_SHOT, MASK_SOLID_BRUSHONLY = MASK_SOLID_BRUSHONLY, MASK_PLAYERSOLID = MASK_PLAYERSOLID,
	MASK_NPCSOLID = MASK_NPCSOLID, MASK_WATER = MASK_WATER, MASK_VISIBLE = MASK_VISIBLE, MASK_OPAQUE = MASK_OPAQUE,
	MASK_ALL = MASK_ALL, MASK_SHOT_HULL = MASK_SHOT_HULL, MASK_PLAYERSOLID_BRUSHONLY = MASK_PLAYERSOLID_BRUSHONLY,
}
local MAT_NAMES = {}
for k, v in pairs(_G) do
	if type(k) == "string" and string.sub(k, 1, 4) == "MAT_" and type(v) == "number" then MAT_NAMES[v] = k end
end

local function resolve_mask(m)
	if m == nil then return MASK_SOLID end
	if type(m) == "number" then return m end
	local v = MASKS[m]
	if not v then A.fail("invalid_params", "unknown mask: " .. tostring(m)) end
	return v
end

local function resolve_filter(f)
	if f == nil then return nil end
	local out = {}
	for _, id in ipairs(f) do
		local e = Entity(id)
		if IsValid(e) then out[#out + 1] = e end
	end
	return out
end

local function surface_of(tr)
	return {
		mat_type = tr.MatType, mat_name = MAT_NAMES[tr.MatType] or J.null,
		surfaceprop = util.GetSurfacePropName(tr.SurfaceProps), surfaceprop_id = tr.SurfaceProps,
		texture = tr.HitTexture,
		entity = (IsValid(tr.Entity) and A.ent_summary(tr.Entity)) or (tr.HitWorld and { id = 0, class = "worldspawn" }) or J.null,
	}
end

local function trace_out(tr)
	local o = surface_of(tr)
	o.hit = tr.Hit
	o.hit_world = tr.HitWorld
	o.hit_sky = tr.HitSky
	o.hit_nodraw = tr.HitNoDraw
	o.pos = vec_out(tr.HitPos)
	o.normal = vec_out(tr.HitNormal)
	o.fraction = tr.Fraction
	o.start_solid = tr.StartSolid
	o.all_solid = tr.AllSolid
	o.fraction_left_solid = tr.FractionLeftSolid
	o.hitgroup = tr.HitGroup
	o.physics_bone = tr.PhysicsBone
	return o
end

function H.trace(args)
	local s, e = A.to_vector(args.start, "start"), A.to_vector(args["end"], "end")
	local tr = util.TraceLine({ start = s, endpos = e, mask = resolve_mask(args.mask), filter = resolve_filter(args.filter) })
	local o = trace_out(tr)
	o.distance = tr.Hit and s:Distance(tr.HitPos) or s:Distance(e)
	return o
end

-- List of surfaces crossed between start and end: entry, exit, thickness, materials.
function H.trace_through(args)
	local s, e = A.to_vector(args.start, "start"), A.to_vector(args["end"], "end")
	local mask = resolve_mask(args.mask)
	local filter = resolve_filter(args.filter)
	local total = s:Distance(e)
	if total < 0.01 then A.fail("invalid_params", "start and end are the same point") end
	local dir = (e - s):GetNormalized()
	local eps = 0.05
	local p = s
	local surfaces = J.array({})
	local current
	local truncated = false
	for _ = 1, (args.max_surfaces or 32) * 2 + 2 do
		if p:Distance(s) >= total then break end
		local tr = util.TraceLine({ start = p, endpos = e, mask = mask, filter = filter })
		if tr.StartSolid then
			-- Inside a solid: exit at FractionLeftSolid.
			local frac = tr.FractionLeftSolid or 0
			local exitpos
			if tr.AllSolid or frac >= 1 then
				exitpos = e
			else
				exitpos = p + (e - p) * frac
			end
			if not current then
				current = { entry = vec_out(p), entry_surface = J.null }
			end
			current.exit = vec_out(exitpos)
			current.thickness = Vector(current.entry[1], current.entry[2], current.entry[3]):Distance(exitpos)
			if exitpos ~= e then
				-- Exit surface, seen from outside (reverse trace).
				local back = util.TraceLine({ start = exitpos + dir * 1, endpos = exitpos - dir * 1, mask = mask, filter = filter })
				current.exit_surface = back.Hit and surface_of(back) or J.null
				current.exit_normal = back.Hit and vec_out(back.HitNormal) or J.null
			else
				current.exit_surface = J.null
				current.reaches_end_inside = true
			end
			-- Precision artefact right after an exit: zero-thickness re-entry with no entry surface -> ignore.
			if not (current.entry_surface == J.null and (current.thickness or 0) < 0.01) then surfaces[#surfaces + 1] = current end
			current = nil
			if exitpos == e then break end
			p = exitpos + dir * 0.5
		elseif tr.Hit then
			current = { entry = vec_out(tr.HitPos), entry_normal = vec_out(tr.HitNormal), entry_surface = surface_of(tr),
				distance_from_start = s:Distance(tr.HitPos) }
			p = tr.HitPos + dir * eps
			if #surfaces >= (args.max_surfaces or 32) then truncated = true break end
		else
			break
		end
	end
	local total_thickness = 0
	for _, srf in ipairs(surfaces) do total_thickness = total_thickness + (srf.thickness or 0) end
	return { surfaces = surfaces, count = #surfaces, total_thickness = total_thickness, distance = total, truncated = truncated }
end
