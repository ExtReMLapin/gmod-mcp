-- Per-client data/ isolation for test clients (Issue #20). Two modes, set by instances_launch:
--   overlay  reads fall back to the real data/, every write, append, delete and rename goes to a private folder
--            data/mcp_overlay/<token>/ that the bridge removes with the client (copy on write);
--   audit    nothing is redirected; each change is written to data/mcp_overlay/<token>/audit.txt.
-- The code must run before any addon's code: the module writes it into a tiny autorun file that runs first in the client
-- and server realms of the test client (mounted folder, see data_overlay.cpp), and the runtime runs it again when it is
-- injected (a second run finds the first and does nothing). It only needs the game's `file` library: no __mcpagent.
--
-- Layout inside data/ (the Lua file library cannot write anywhere else):
--   mcp_overlay/<token>/f/<name>      the new content of <name>
--   mcp_overlay/<token>/w/<name>.txt  a tombstone: <name> was deleted by the client
--   mcp_overlay/<token>/audit.txt     audit mode: "<op>\t<existed 0|1>\t<name>" lines, op W (write), D (delete), M (mkdir)
-- What it does not cover: a reference to file.Write & co. taken before this code ran, writes that are not made through the
-- file library (console commands such as `condump`, the sql/cookie databases), a directory rename.
local M = (function()
	local M = {}
	local STATE = "__mcpagent_data_overlay_state"

	local function is_str(v) return type(v) == "string" end

	-- "a/b.txt" from "A\\b.txt" or "/a//b.txt"; nil when the name must not be handled here (empty, ".." segment, not a string).
	local function norm(name)
		if not is_str(name) or name == "" then return nil end
		name = string.gsub(string.gsub(name, "\\", "/"), "/+", "/")
		name = string.gsub(name, "^/", "")
		name = string.gsub(name, "/$", "")
		if name == "" then return nil end
		for seg in string.gmatch(name, "[^/]+") do
			if seg == ".." or seg == "." then return nil end
		end
		return name
	end

	-- The data-folder name a (name, path) pair designates: path "DATA", or data/<name> through "GAME" / "MOD" (and no path at
	-- all, which reads through GAME). `nil_is_data`: file.Delete's path defaults to DATA.
	local function data_name(name, path, nil_is_data)
		local n = norm(name)
		if not n then return nil end
		if path == nil then
			if nil_is_data then return n end
			path = "GAME"
		end
		if not is_str(path) then return nil end
		local p = string.lower(path)
		if p == "data" then return n end
		if p == "game" or p == "mod" then
			local rest = string.match(n, "^[Dd][Aa][Tt][Aa]/(.+)$")
			if rest then return rest end
		end
		return nil
	end

	function M.install(cfg, by)
		if rawget(_G, STATE) then return rawget(_G, STATE) end
		if type(file) ~= "table" or type(cfg) ~= "table" then return nil end
		local token = cfg.token
		if not is_str(token) or not string.match(token, "^[%w_%-]+$") or #token > 40 then return nil end
		local mode = cfg.mode == "audit" and "audit" or "overlay"
		local O = {}
		for _, k in ipairs({ "Exists", "IsDir", "Find", "Read", "Open", "Write", "Append", "Delete", "CreateDir", "Rename", "Size", "Time", "AsyncRead" }) do
			O[k] = file[k]
		end
		if not (O.Exists and O.Read and O.Write and O.Append and O.Delete and O.CreateDir and O.Open and O.Find and O.IsDir) then return nil end

		local root = "mcp_overlay/" .. token
		local st = { mode = mode, token = token, root = root, installed_by = by or "unknown", ops = { write = 0, append = 0, delete = 0, mkdir = 0, rename = 0, read_overlay = 0 },
			original = O }
		_G[STATE] = st

		local function ovl(n) return root .. "/f/" .. n end
		local function wh(n) return root .. "/w/" .. n .. ".txt" end
		local function parent_of(n) return string.match(n, "^(.*)/[^/]+$") end
		local function ensure_dir(p) -- p relative to data/, creates every level
			local acc = nil
			for seg in string.gmatch(p, "[^/]+") do
				acc = acc and (acc .. "/" .. seg) or seg
				if not O.IsDir(acc, "DATA") then O.CreateDir(acc) end
			end
		end

		if mode == "audit" then
			local last, made = {}, false
			local function log(op, n, is_dir)
				if last[n] == op then return end
				last[n] = op
				local existed = (is_dir and O.IsDir(n, "DATA")) or (not is_dir and O.Exists(n, "DATA"))
				if not made then ensure_dir(root) made = true end
				O.Append(root .. "/audit.txt", op .. "\t" .. (existed and 1 or 0) .. "\t" .. n .. "\n")
			end
			file.Write = function(name, ...)
				local n = data_name(name, "DATA")
				if n then log("W", n) st.ops.write = st.ops.write + 1 end
				return O.Write(name, ...)
			end
			file.Append = function(name, ...)
				local n = data_name(name, "DATA")
				if n then log("W", n) st.ops.append = st.ops.append + 1 end
				return O.Append(name, ...)
			end
			file.Delete = function(name, path, ...)
				local n = data_name(name, path, true)
				if n then log("D", n, O.IsDir(n, "DATA")) st.ops.delete = st.ops.delete + 1 end
				return O.Delete(name, path, ...)
			end
			file.CreateDir = function(name, ...)
				local n = data_name(name, "DATA")
				if n then log("M", n, true) st.ops.mkdir = st.ops.mkdir + 1 end
				return O.CreateDir(name, ...)
			end
			if O.Rename then
				file.Rename = function(from, to, ...)
					local a, b = data_name(from, "DATA"), data_name(to, "DATA")
					if a then log("D", a) end
					if b then log("W", b) end
					st.ops.rename = st.ops.rename + 1
					return O.Rename(from, to, ...)
				end
			end
			file.Open = function(name, mode_, path, ...)
				local n = data_name(name, path or "GAME")
				if n and is_str(mode_) and string.find(mode_, "[wa+]") then log("W", n) st.ops.write = st.ops.write + 1 end
				return O.Open(name, mode_, path, ...)
			end
			return st
		end

		-- ---- overlay mode ----
		local function deleted(n) return O.Exists(wh(n), "DATA") end
		local function exists(n) return not deleted(n) and (O.Exists(ovl(n), "DATA") or O.Exists(n, "DATA")) end
		local function isdir(n) return not deleted(n) and (O.IsDir(ovl(n), "DATA") or O.IsDir(n, "DATA")) end
		local function clear_tombstone(n) if O.Exists(wh(n), "DATA") then O.Delete(wh(n), "DATA") end end
		-- The file's current content as the client sees it, nil when it does not exist.
		local function logical_read(n)
			if deleted(n) then return nil end
			if O.Exists(ovl(n), "DATA") and not O.IsDir(ovl(n), "DATA") then return O.Read(ovl(n), "DATA") end
			if O.Exists(n, "DATA") and not O.IsDir(n, "DATA") then return O.Read(n, "DATA") end
			return nil
		end
		-- Where a write to <n> goes (the overlay copy), or nil when the client could not write it either (no parent folder).
		local function prepare_write(n)
			local par = parent_of(n)
			if par and not isdir(par) then return nil end
			ensure_dir(root .. "/f" .. (par and ("/" .. par) or ""))
			clear_tombstone(n)
			return ovl(n)
		end
		local function tombstone(n)
			ensure_dir(root .. "/w" .. (parent_of(n) and ("/" .. parent_of(n)) or ""))
			O.Write(wh(n), "x")
		end
		-- Where a read of <n> is served from: "ovl", "real", or nil (deleted / absent).
		local function source(n)
			if deleted(n) then return nil end
			if O.Exists(ovl(n), "DATA") then return "ovl" end
			if O.Exists(n, "DATA") then return "real" end
			return nil
		end

		file.Exists = function(name, path, ...)
			local n = data_name(name, path)
			if not n then return O.Exists(name, path, ...) end
			return exists(n)
		end
		file.IsDir = function(name, path, ...)
			local n = data_name(name, path)
			if not n then return O.IsDir(name, path, ...) end
			return isdir(n)
		end
		file.Read = function(name, path, ...)
			local n = data_name(name, path)
			if not n then return O.Read(name, path, ...) end
			local s = source(n)
			if s == "ovl" then st.ops.read_overlay = st.ops.read_overlay + 1 return O.Read(ovl(n), "DATA", ...) end
			if s == "real" then return O.Read(n, "DATA", ...) end
			return nil
		end
		if O.Size then
			file.Size = function(name, path, ...)
				local n = data_name(name, path)
				if not n then return O.Size(name, path, ...) end
				local s = source(n)
				if s == "ovl" then return O.Size(ovl(n), "DATA", ...) end
				if s == "real" then return O.Size(n, "DATA", ...) end
				return -1
			end
		end
		if O.Time then
			file.Time = function(name, path, ...)
				local n = data_name(name, path)
				if not n then return O.Time(name, path, ...) end
				local s = source(n)
				if s == "ovl" then return O.Time(ovl(n), "DATA", ...) end
				if s == "real" then return O.Time(n, "DATA", ...) end
				return 0
			end
		end
		file.Write = function(name, content, ...)
			local n = data_name(name, "DATA")
			if not n then return O.Write(name, content, ...) end
			st.ops.write = st.ops.write + 1
			local p = prepare_write(n)
			if p then return O.Write(p, content, ...) end
		end
		file.Append = function(name, content, ...)
			local n = data_name(name, "DATA")
			if not n then return O.Append(name, content, ...) end
			st.ops.append = st.ops.append + 1
			-- The real file's content is copied up first (unless the client deleted it before): an append adds to it.
			local keep = not O.Exists(ovl(n), "DATA") and exists(n)
			local old = keep and logical_read(n) or nil
			local p = prepare_write(n)
			if p then
				if old then O.Write(p, old) end
				return O.Append(p, content, ...)
			end
		end
		file.CreateDir = function(name, ...)
			local n = data_name(name, "DATA")
			if not n then return O.CreateDir(name, ...) end
			st.ops.mkdir = st.ops.mkdir + 1
			if isdir(n) then return end
			ensure_dir(root .. "/f/" .. n)
			clear_tombstone(n)
		end
		file.Delete = function(name, path, ...)
			local n = data_name(name, path, true)
			if not n then return O.Delete(name, path, ...) end
			st.ops.delete = st.ops.delete + 1
			local had = exists(n)
			if O.Exists(ovl(n), "DATA") then O.Delete(ovl(n), "DATA") end
			if had and O.Exists(n, "DATA") then tombstone(n) end
			return had
		end
		if O.Rename then
			file.Rename = function(from, to, ...)
				local a, b = data_name(from, "DATA"), data_name(to, "DATA")
				if not a or not b then return O.Rename(from, to, ...) end
				st.ops.rename = st.ops.rename + 1
				local c = logical_read(a)
				if c == nil then return false end -- missing, or a folder: renaming a folder is not redirected
				local p = prepare_write(b)
				if not p then return false end
				O.Write(p, c)
				file.Delete(a, "DATA")
				return true
			end
		end
		file.Find = function(pattern, path, sorting, ...)
			local pn = norm(pattern)
			local n = data_name(pattern, path)
			if not n or not pn then return O.Find(pattern, path, sorting, ...) end
			local dirpart = parent_of(n)
			local rf, rd = O.Find(n, "DATA")
			local of, od = O.Find(ovl(n), "DATA")
			local seen, files, dirs = {}, {}, {}
			local function add(list, entries, is_dir)
				for _, e in ipairs(entries or {}) do
					local key = string.lower(e)
					local full = dirpart and (dirpart .. "/" .. e) or e
					local hidden = (not dirpart and is_dir and key == "mcp_overlay") or deleted(full)
					if not seen[key] and not hidden then seen[key] = true list[#list + 1] = e end
				end
			end
			add(files, of, false) add(files, rf, false)
			add(dirs, od, true) add(dirs, rd, true)
			local s = is_str(sorting) and string.lower(sorting) or "nameasc"
			local cmp
			if s == "namedesc" then cmp = function(a, b) return string.lower(a) > string.lower(b) end
			elseif s == "nameasc" then cmp = function(a, b) return string.lower(a) < string.lower(b) end end
			if cmp then table.sort(files, cmp) table.sort(dirs, cmp) end
			return files, dirs
		end
		file.Open = function(name, mode_, path, ...)
			local n = data_name(name, path or "GAME")
			if not n or not is_str(mode_) then return O.Open(name, mode_, path, ...) end
			if string.find(mode_, "[wa+]") then
				local fresh = string.sub(mode_, 1, 1) == "w"
				local keep = not fresh and exists(n) and not O.Exists(ovl(n), "DATA")
				local content = keep and logical_read(n) or nil
				local p = prepare_write(n)
				if not p then return nil end
				if content then O.Write(p, content) end
				st.ops.write = st.ops.write + 1
				return O.Open(p, mode_, "DATA", ...)
			end
			local s = source(n)
			if s == "ovl" then return O.Open(ovl(n), mode_, "DATA", ...) end
			if s == "real" then return O.Open(n, mode_, "DATA", ...) end
			return nil
		end
		if O.AsyncRead then
			file.AsyncRead = function(name, path, callback, sync, ...)
				local n = data_name(name, path)
				if not n then return O.AsyncRead(name, path, callback, sync, ...) end
				local s = source(n)
				if s == nil then
					if callback then callback(name, path, rawget(_G, "FSASYNC_ERR_FILEOPEN") or -1, nil) end
					return 0
				end
				local target = s == "ovl" and ovl(n) or n
				return O.AsyncRead(target, "DATA", function(_, _, status, data)
					if callback then return callback(name, path, status, data) end
				end, sync, ...)
			end
		end
		return st
	end

	-- Puts the file library back (tests).
	function M.uninstall()
		local st = rawget(_G, STATE)
		if not st then return false end
		for k, f in pairs(st.original) do file[k] = f end
		_G[STATE] = nil
		return true
	end

	function M.status()
		local st = rawget(_G, STATE)
		if not st then return { installed = false } end
		return { installed = true, mode = st.mode, token = st.token, root = st.root, installed_by = st.installed_by, ops = st.ops }
	end

	return M
end)()

-- Entry points. The early autorun file sets __mcpagent_data_cfg ({mode, token}) before this code; the runtime gets the same
-- from the command line through the native table. The menu state has no addons: not touched.
if rawget(_G, "__mcpagent_data_overlay_state") == nil and not rawget(_G, "MENU_DLL") then
	local early = rawget(_G, "__mcpagent_data_cfg")
	local nat = rawget(_G, "__mcpagent_native")
	local cfg = early or (type(nat) == "table" and nat.data_overlay) or nil
	if cfg then M.install(cfg, early and "early" or "runtime") end
end
if rawget(_G, "__mcpagent") then
	__mcpagent.data_overlay = M
end
