-- gmod-mcp: rendering (screenshots, arbitrary viewpoint, debug primitives). Client realm only
-- (the menu state has no render.Capture).
local A = __mcpagent
local J = A.json
local H = A.handlers
if A.realm ~= "client" then return end

-- One-time notice to the local player (after installation): HUD notification, chat line and a sound, after a delay so
-- the player is really in game.
function H.notify(args)
	local text = tostring(args.text or "")
	timer.Simple(args.delay or 5, function()
		if notification and notification.AddLegacy then notification.AddLegacy(text, NOTIFY_GENERIC, 15) end
		if chat and chat.AddText then chat.AddText(Color(80, 200, 255), "[gmod-mcp] ", Color(255, 255, 255), text) end
		surface.PlaySound("buttons/button15.wav")
	end)
	return { scheduled = true, delay = args.delay or 5 }
end

-- Deferred capture: the request runs in a render hook, the DLL polls for the result.
A.capture = A.capture or {}
local cap = A.capture

local function finish(data, info)
	cap.pending = nil
	if not data or #data == 0 then
		cap.result = { error = "render.Capture returned empty data" }
	else
		cap.result = info
		cap.data = data
	end
end

local function do_capture(req)
	local fmt = req.format == "png" and "png" or "jpeg"
	local sw, sh = ScrW(), ScrH()
	local w, h = req.w or sw, req.h or sh
	local info = { format = fmt, w = w, h = h, screen_w = sw, screen_h = sh, realm = A.realm }
	if A.realm == "client" and (w ~= sw or h ~= sh or req.origin) then
		-- Arbitrary dimensions (or explicit viewpoint): render into a render target of the requested size.
		local rt = GetRenderTargetEx("mcpagent_rt_" .. w .. "x" .. h, w, h, RT_SIZE_LITERAL, MATERIAL_RT_DEPTH_SEPARATE, 0, 0, IMAGE_FORMAT_RGB888)
		local ply = LocalPlayer()
		render.PushRenderTarget(rt)
		render.Clear(0, 0, 0, 255, true, true)
		render.RenderView({
			origin = req.origin or (IsValid(ply) and ply:EyePos()) or Vector(),
			angles = req.angles or (IsValid(ply) and ply:EyeAngles()) or Angle(),
			fov = req.fov or 90, x = 0, y = 0, w = w, h = h, aspect = w / h,
			-- Show the local player only from an arbitrary viewpoint (otherwise its head blocks the view).
			drawhud = false, drawviewmodel = false, dopostprocess = true, drawviewer = req.origin ~= nil,
		})
		local data = render.Capture({ format = fmt, x = 0, y = 0, w = w, h = h, quality = req.quality or 90, alpha = false })
		render.PopRenderTarget()
		info.method = "render_target"
		finish(data, info)
	else
		-- Screen size: capture the back buffer; in the menu, a smaller size is a top-left crop.
		w, h = math.min(w, sw), math.min(h, sh)
		info.w, info.h = w, h
		info.method = A.realm == "menu" and "menu_backbuffer" or "backbuffer"
		local data = render.Capture({ format = fmt, x = 0, y = 0, w = w, h = h, quality = req.quality or 90, alpha = false })
		finish(data, info)
	end
end

local HOOK = A.realm == "client" and "PostRender" or "DrawOverlay"
hook.Add(HOOK, "__mcpagent_capture", function()
	local req = cap.pending
	if not req then return end
	if req.wait_frames and req.wait_frames > 0 then
		req.wait_frames = req.wait_frames - 1
		return
	end
	local ok, err = pcall(do_capture, req)
	if not ok then
		cap.pending = nil
		cap.result = { error = tostring(err) }
	end
end)

function H.capture_request(args)
	if cap.pending then A.fail("busy", "a capture is already in progress") end
	cap.result, cap.data = nil, nil
	local req = { format = args.format, quality = args.quality, w = args.w, h = args.h, wait_frames = args.wait_frames or 1 }
	if args.origin then
		req.origin = A.to_vector(args.origin, "pos")
		req.angles = A.to_angle(args.angles or { 0, 0, 0 }, "ang")
		req.fov = args.fov
	end
	cap.pending = req
	return { requested = true, hook = HOOK }
end

function H.capture_poll(args)
	if cap.pending then return { done = false } end
	local r = cap.result
	if not r then A.fail("not_found", "no capture requested") end
	cap.result = nil
	if r.error then A.fail("capture_failed", r.error) end
	A.blob = cap.data
	cap.data = nil
	r.done = true
	r.bytes = #A.blob
	return r
end

-- Arbitrary view at screen size: temporary CalcView, removed after the capture.
function H.view_set(args)
	if A.realm ~= "client" then A.fail("wrong_realm", "view_set: requires the client realm") end
	local origin, angles = A.to_vector(args.pos, "pos"), A.to_angle(args.ang, "ang")
	local fov = args.fov or 90
	hook.Add("CalcView", "__mcpagent_view", function(ply, pos, ang, f)
		return { origin = origin, angles = angles, fov = fov, drawviewer = true }
	end)
	return { installed = true }
end

function H.view_clear(args)
	hook.Remove("CalcView", "__mcpagent_view")
	local t = hook.GetTable().CalcView
	return { removed = not (t and t.__mcpagent_view) }
end

-- Debug primitives (3D Lua rendering, visible in captures). Duration 0 = until debug_clear.
A.dd = A.dd or {}
local dd = A.dd

local function col(c)
	if type(c) == "table" then return Color(c[1] or c.r or 255, c[2] or c.g or 255, c[3] or c.b or 255, c[4] or c.a or 255) end
	return Color(255, 0, 0, 255)
end

if A.realm == "client" then
	hook.Add("PostDrawTranslucentRenderables", "__mcpagent_debugdraw", function(depth, sky)
		if sky or #dd == 0 then return end
		local now = CurTime()
		local alive = {}
		for _, p in ipairs(dd) do
			if p.expire == 0 or p.expire > now then
				alive[#alive + 1] = p
				local z = not p.ignore_z
				if p.type == "line" then
					render.DrawLine(p.from, p.to, p.color, z)
				elseif p.type == "box" then
					render.DrawWireframeBox(p.pos, p.ang, p.mins, p.maxs, p.color, z)
				elseif p.type == "sphere" then
					render.DrawWireframeSphere(p.pos, p.radius, 12, 12, p.color, z)
				elseif p.type == "text" then
					local ang = EyeAngles()
					ang:RotateAroundAxis(ang:Up(), -90)
					ang:RotateAroundAxis(ang:Forward(), 90)
					cam.Start3D2D(p.pos, ang, p.scale)
					cam.IgnoreZ(p.ignore_z)
					draw.SimpleText(p.text, "DermaLarge", 0, 0, p.color, TEXT_ALIGN_CENTER, TEXT_ALIGN_CENTER)
					cam.IgnoreZ(false)
					cam.End3D2D()
				end
			end
		end
		if #alive ~= #dd then
			for i = #dd, 1, -1 do dd[i] = nil end
			for i, p in ipairs(alive) do dd[i] = p end
		end
	end)
end

function H.debug_draw(args)
	if A.realm ~= "client" then A.fail("wrong_realm", "debug_draw: requires the client realm (load a map)") end
	local dur = args.duration or 10
	local expire = dur > 0 and (CurTime() + dur) or 0
	local n = 0
	for i, p in ipairs(args.primitives or {}) do
		local o = { type = p.type, color = col(p.color), ignore_z = p.ignore_z ~= false, expire = expire }
		if p.type == "line" then
			o.from, o.to = A.to_vector(p.from, "from"), A.to_vector(p.to, "to")
		elseif p.type == "box" then
			if p.min and p.max then
				local mn, mx = A.to_vector(p.min, "min"), A.to_vector(p.max, "max")
				o.pos, o.ang, o.mins, o.maxs = Vector(0, 0, 0), Angle(0, 0, 0), mn, mx
			else
				o.pos = A.to_vector(p.pos, "pos")
				o.ang = p.ang and A.to_angle(p.ang, "ang") or Angle(0, 0, 0)
				o.mins = A.to_vector(p.mins or { -8, -8, -8 }, "mins")
				o.maxs = A.to_vector(p.maxs or { 8, 8, 8 }, "maxs")
			end
		elseif p.type == "sphere" then
			o.pos, o.radius = A.to_vector(p.pos, "pos"), p.radius or 8
		elseif p.type == "text" then
			o.pos, o.text, o.scale = A.to_vector(p.pos, "pos"), tostring(p.text or ""), p.scale or 0.25
		else
			A.fail("invalid_params", "primitive " .. i .. ": unknown type " .. tostring(p.type) .. " (line, box, sphere, text)")
		end
		dd[#dd + 1] = o
		n = n + 1
	end
	return { added = n, active = #dd, expire_in_s = dur }
end

function H.debug_clear(args)
	local n = #dd
	for i = #dd, 1, -1 do dd[i] = nil end
	return { cleared = n }
end
