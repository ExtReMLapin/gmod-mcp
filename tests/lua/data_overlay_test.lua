-- Issue #20: the data/ overlay and audit of test clients (src/lua/12_data_overlay.lua), without the game, on a fake `file`
-- library that behaves like the game's for the data folder. Run: luajit tests/lua/data_overlay_test.lua src/lua
local dir = arg[1] or "src/lua"
local fails, passes = 0, 0
local function check(cond, what)
	if cond then passes = passes + 1 else fails = fails + 1 print("FAIL " .. what) end
end

-- A fake data/ folder: case-insensitive names, file.Write and Append fail silently without the parent folder (as in the game).
local function new_fs()
	local fs = { files = {}, dirs = {}, names = {}, writes = {} }
	local function key(p) return string.lower(p) end
	local function parent(p) return string.match(p, "^(.*)/[^/]+$") end
	local F = {}
	function F.Exists(n, path) local k = key(n) return fs.files[k] ~= nil or fs.dirs[k] == true end
	function F.IsDir(n, path) return fs.dirs[key(n)] == true end
	function F.Read(n, path) return fs.files[key(n)] end
	function F.Size(n, path) local c = fs.files[key(n)] return c and #c or -1 end
	function F.Time(n, path) return fs.files[key(n)] and 100 or 0 end
	-- As in the game (lua/includes/extensions/file.lua): file.Write and file.Append are Lua functions that go through
	-- the global file.Open, so a wrapper of file.Open sees the calls its own file.Write makes (Issue #20, found in the game).
	function F.Write(n, c)
		local h = file.Open(n, "wb", "DATA")
		if not h then return end
		h:Write(c) h:Close()
	end
	function F.Append(n, c)
		local h = file.Open(n, "ab", "DATA")
		if not h then return end
		h:Write(c) h:Close()
	end
	function F.Delete(n, path)
		local k = key(n)
		if fs.files[k] then fs.files[k] = nil return true end
		if fs.dirs[k] then fs.dirs[k] = nil return true end
		return false
	end
	function F.CreateDir(n)
		local acc
		for seg in string.gmatch(n, "[^/]+") do acc = acc and (acc .. "/" .. seg) or seg fs.dirs[key(acc)] = true fs.names[key(acc)] = seg end
	end
	function F.Rename(a, b) local c = fs.files[key(a)] if not c then return false end fs.files[key(b)] = c fs.files[key(a)] = nil fs.names[key(b)] = b return true end
	function F.Find(pattern, path, sorting)
		local d, pat = string.match(pattern, "^(.*)/([^/]*)$")
		if not d then d, pat = "", pattern end
		local lp = "^" .. string.gsub(string.gsub(string.lower(pat), "[%^%$%(%)%%%.%[%]%+%-%?]", "%%%0"), "%*", ".*") .. "$"
		local files, dirs = {}, {}
		local function scan(tbl, out)
			for k in pairs(tbl) do
				local par, base = string.match(k, "^(.*)/([^/]+)$")
				if not par then par, base = "", k end
				if par == string.lower(d) and string.match(base, lp) then
					local nm = fs.names[k] or base
					out[#out + 1] = string.match(nm, "([^/]+)$")
				end
			end
		end
		scan(fs.files, files) scan(fs.dirs, dirs)
		table.sort(files) table.sort(dirs)
		return files, dirs
	end
	function F.Open(n, mode, path)
		if string.find(mode, "w") then
			local p = parent(n)
			if p and not fs.dirs[key(p)] then return nil end
			fs.files[key(n)] = "" fs.names[key(n)] = n fs.writes[#fs.writes + 1] = n
		elseif string.find(mode, "a") then
			local p = parent(n)
			if p and not fs.dirs[key(p)] then return nil end
			fs.files[key(n)] = fs.files[key(n)] or "" fs.names[key(n)] = n fs.writes[#fs.writes + 1] = n
		elseif not fs.files[key(n)] then return nil end
		local h = { path = n, mode = mode }
		function h:Write(s) fs.files[key(n)] = fs.files[key(n)] .. s end
		function h:Read() return fs.files[key(n)] end
		function h:Close() end
		return h
	end
	function F.AsyncRead(n, path, cb, sync)
		cb(n, path, 0, fs.files[key(n)])
		return 0
	end
	return fs, F
end

local function load_overlay()
	local f = assert(loadfile(dir .. "/12_data_overlay.lua"))
	f()
end
local function fresh(cfg)
	local fs, F = new_fs()
	_G.file = F
	for k, v in pairs(F) do file[k] = v end
	_G.__mcpagent_data_cfg = nil
	_G.__mcpagent_native = nil
	_G.__mcpagent_data_overlay_state = nil
	_G.MENU_DLL = nil
	return fs, F
end
local function boot(cfg, how)
	if how == "native" then _G.__mcpagent_native = { data_overlay = cfg } else _G.__mcpagent_data_cfg = cfg end
	load_overlay()
end

-- A real data folder of the user: some addon cache.
local function seed(fs)
	fs.dirs["cache"] = true fs.names["cache"] = "cache"
	fs.dirs["cache/gm_construct"] = true fs.names["cache/gm_construct"] = "gm_construct"
	fs.files["cache/index.txt"] = "v1" fs.names["cache/index.txt"] = "index.txt"
	fs.files["cache/gm_construct/t1.dat"] = "tile1" fs.names["cache/gm_construct/t1.dat"] = "t1.dat"
	fs.files["cache/gm_construct/t2.dat"] = "tile2" fs.names["cache/gm_construct/t2.dat"] = "t2.dat"
	fs.files["user.txt"] = "hello" fs.names["user.txt"] = "user.txt"
end
local function snapshot(fs)
	local t = {}
	for k, v in pairs(fs.files) do if not string.find(k, "^mcp_overlay") then t[#t + 1] = k .. "=" .. v end end
	for k in pairs(fs.dirs) do if not string.find(k, "^mcp_overlay") then t[#t + 1] = "dir:" .. k end end
	table.sort(t)
	return table.concat(t, "|")
end
local function has_overlay_files(fs)
	for k in pairs(fs.files) do if string.find(k, "^mcp_overlay") then return true end end
	return false
end

-- ---------------------------------------------------------------------------------------------------------------- install
do
	local fs = fresh()
	boot(nil)
	check(not __mcpagent_data_overlay_state, "no configuration: nothing installed")
	local orig_write = file.Write
	boot({ mode = "overlay", token = "bad token!" })
	check(file.Write == orig_write, "a token with a space is refused")
	boot({ mode = "overlay", token = "../x" })
	check(file.Write == orig_write, "a token with a path is refused")
	_G.MENU_DLL = true
	boot({ mode = "overlay", token = "t1" })
	check(file.Write == orig_write, "the menu state is never touched")
	_G.MENU_DLL = nil
end

-- A second load (the runtime injection after the early autorun file) finds the first and does nothing.
do
	local fs = fresh()
	boot({ mode = "overlay", token = "t1" })
	local w = file.Write
	local st = __mcpagent_data_overlay_state
	check(st and st.installed_by == "early", "installed by the early file")
	boot({ mode = "overlay", token = "t1" }, "native")
	check(file.Write == w and __mcpagent_data_overlay_state == st, "a second load keeps the first install")
	check(__mcpagent_data_overlay_state.installed_by == "early", "installed_by stays early")
end
do
	fresh()
	boot({ mode = "overlay", token = "t2" }, "native")
	check(__mcpagent_data_overlay_state and __mcpagent_data_overlay_state.installed_by == "runtime", "installed by the runtime when no early file ran")
end

-- ---------------------------------------------------------------------------------------------------------------- overlay
do
	local fs = fresh()
	seed(fs)
	local before = snapshot(fs)
	boot({ mode = "overlay", token = "t1" })

	-- Reads fall back to the real data.
	check(file.Exists("cache/index.txt", "DATA") and file.Read("cache/index.txt", "DATA") == "v1", "read falls back to the real file")
	check(file.IsDir("cache/gm_construct", "DATA"), "IsDir sees a real folder")
	check(file.Size("user.txt", "DATA") == 5 and file.Time("user.txt", "DATA") == 100, "Size and Time of a real file")
	check(file.Read("data/user.txt", "GAME") == "hello", "data/ through GAME is the same file")
	check(file.Read("cache\\index.txt", "DATA") == "v1", "backslashes and case are accepted")

	-- Writes go to the overlay, the real data is untouched.
	file.Write("cache/index.txt", "v2")
	check(file.Read("cache/index.txt", "DATA") == "v2", "the client reads its own write")
	check(fs.files["cache/index.txt"] == "v1", "the real file is untouched after a write")
	-- Issue #20, found in the game: the overlay's own call to the engine's file.Write went through the wrapped file.Open and
	-- was redirected a second time (data/mcp_overlay/<token>/f/mcp_overlay/<token>/f/...), so the client never read its writes.
	check(fs.files["mcp_overlay/t1/f/cache/index.txt"] == "v2", "the overlay copy is at <root>/f/<name>, not nested")
	check(not fs.files["mcp_overlay/t1/f/mcp_overlay/t1/f/cache/index.txt"], "no second redirection of the overlay's own writes")
	check(file.Size("cache/index.txt", "DATA") == 2, "Size follows the overlay copy")
	file.Write("cache/gm_construct/new.dat", "n")
	check(file.Exists("cache/gm_construct/new.dat", "DATA") and not fs.files["cache/gm_construct/new.dat"], "a new file exists for the client only")
	file.Write("nofolder/x.txt", "n")
	check(not file.Exists("nofolder/x.txt", "DATA"), "a write without its parent folder fails, as in the game")

	-- Append copies the real content up first.
	file.Append("cache/gm_construct/t1.dat", "+more")
	check(file.Read("cache/gm_construct/t1.dat", "DATA") == "tile1+more", "append adds to the real content")
	check(fs.files["cache/gm_construct/t1.dat"] == "tile1", "the real file is untouched after an append")
	file.Append("brand_new.txt", "a") file.Append("brand_new.txt", "b")
	check(file.Read("brand_new.txt", "DATA") == "ab", "append to a new file")

	-- Delete: the client no longer sees the real file, the real file stays.
	file.Delete("cache/gm_construct/t2.dat", "DATA")
	check(not file.Exists("cache/gm_construct/t2.dat", "DATA") and file.Read("cache/gm_construct/t2.dat", "DATA") == nil, "a deleted real file is gone for the client")
	check(fs.files["cache/gm_construct/t2.dat"] == "tile2", "the real file is still there after a delete")
	check(file.Size("cache/gm_construct/t2.dat", "DATA") == -1 and file.Time("cache/gm_construct/t2.dat", "DATA") == 0, "Size and Time of a deleted file")
	file.Write("cache/gm_construct/t2.dat", "again")
	check(file.Read("cache/gm_construct/t2.dat", "DATA") == "again", "a file written again after a delete is back")
	file.Delete("cache/gm_construct/new.dat")  -- no path: DATA
	check(not file.Exists("cache/gm_construct/new.dat", "DATA"), "delete without a path works on DATA")

	-- A cleanup that deletes everything it finds (the case of issue #20): the user's cache survives.
	local files = file.Find("cache/gm_construct/*", "DATA")
	for _, f in ipairs(files) do file.Delete("cache/gm_construct/" .. f, "DATA") end
	file.Delete("cache/gm_construct", "DATA")
	file.Delete("cache/index.txt", "DATA")
	check(not file.IsDir("cache/gm_construct", "DATA") and not file.Exists("cache/index.txt", "DATA"), "the client's cleanup removed everything for itself")
	check(snapshot(fs) == before, "the real data folder is exactly as before the writes and the cleanup")

	-- CreateDir and Rename.
	file.CreateDir("fresh/sub")
	check(file.IsDir("fresh/sub", "DATA") and not fs.dirs["fresh"], "CreateDir creates in the overlay only")
	file.Write("fresh/sub/a.txt", "A")
	check(file.Read("fresh/sub/a.txt", "DATA") == "A", "a file in a new folder")
	check(file.Rename("fresh/sub/a.txt", "fresh/sub/b.txt") == true, "Rename answers true")
	check(not file.Exists("fresh/sub/a.txt", "DATA") and file.Read("fresh/sub/b.txt", "DATA") == "A", "Rename moves the content")
	check(file.Rename("user.txt", "user2.txt") == true and file.Read("user2.txt", "DATA") == "hello" and not file.Exists("user.txt", "DATA"), "Rename of a real file")
	check(fs.files["user.txt"] == "hello" and not fs.files["user2.txt"], "Rename leaves the real data alone")
	check(file.Rename("missing.txt", "x.txt") == false, "Rename of a missing file answers false")
	check(snapshot(fs) == before, "the real data folder is still as before")
	check(has_overlay_files(fs), "the changes went into the overlay folder")

end

do
	local fs = fresh()
	seed(fs)
	local before = snapshot(fs)
	boot({ mode = "overlay", token = "t3" })
	local h = file.Open("cache/index.txt", "wb", "DATA")
	check(h ~= nil, "Open for write works")
	h:Write("w") h:Close()
	check(file.Read("cache/index.txt", "DATA") == "w" and fs.files["cache/index.txt"] == "v1", "Open for write goes to the overlay")
	h = file.Open("cache/gm_construct/t1.dat", "ab", "DATA")
	h:Write("X") h:Close()
	check(file.Read("cache/gm_construct/t1.dat", "DATA") == "tile1X" and fs.files["cache/gm_construct/t1.dat"] == "tile1", "Open for append copies up")
	h = file.Open("user.txt", "rb", "DATA")
	check(h ~= nil and h:Read() == "hello", "Open for read reads the real file")
	check(file.Open("nothing.txt", "rb", "DATA") == nil, "Open for read of a missing file")
	file.Delete("user.txt", "DATA")
	check(file.Open("user.txt", "rb", "DATA") == nil, "Open for read of a deleted file")
	-- AsyncRead.
	local got
	file.AsyncRead("cache/index.txt", "DATA", function(n, p, status, data) got = { n, status, data } end)
	check(got and got[1] == "cache/index.txt" and got[3] == "w", "AsyncRead serves the overlay copy under the asked name")
	got = nil
	file.AsyncRead("user.txt", "DATA", function(n, p, status, data) got = { n, status, data } end)
	check(got and got[1] == "user.txt" and got[3] == nil and got[2] ~= 0, "AsyncRead of a deleted file fails")
	check(fs.files["user.txt"] == "hello" and fs.files["cache/index.txt"] == "v1", "the real files are untouched")
end

-- Find merges the real and the overlay entries, hides tombstoned ones and the overlay folder.
do
	local fs = fresh()
	seed(fs)
	boot({ mode = "overlay", token = "t4" })
	file.Write("cache/gm_construct/t3.dat", "3")
	file.Delete("cache/gm_construct/t1.dat", "DATA")
	local files = file.Find("cache/gm_construct/*", "DATA")
	check(#files == 2 and files[1] == "t2.dat" and files[2] == "t3.dat", "Find: real + new - deleted, sorted")
	files = file.Find("cache/gm_construct/*", "DATA", "namedesc")
	check(files[1] == "t3.dat", "Find: namedesc")
	file.Write("cache/gm_construct/t2.dat", "TWO")
	files = file.Find("cache/gm_construct/*", "DATA")
	check(#files == 2, "Find: a file in both the real data and the overlay is listed once")
	file.CreateDir("cache/extra")
	local _, dirs = file.Find("cache/*", "DATA")
	check(#dirs == 2 and dirs[1] == "extra" and dirs[2] == "gm_construct", "Find: folders of both")
	_, dirs = file.Find("*", "DATA")
	local seen_overlay = false
	for _, d in ipairs(dirs) do seen_overlay = seen_overlay or d == "mcp_overlay" end
	check(not seen_overlay, "Find: the overlay folder is hidden at the top level")
	check(#file.Find("cache/*", "DATA") == 1, "Find: files of a folder")
	-- Other path IDs are not touched.
	local calls = 0
	local base = file.Exists
	check(file.Exists("lua/includes/init.lua", "GAME") == false, "another path ID goes to the real function")
end

-- Names that must reach the real function untouched.
do
	local fs = fresh()
	seed(fs)
	boot({ mode = "overlay", token = "t5" })
	file.Write("../escape.txt", "x")
	check(not has_overlay_files(fs), "a name with .. is left to the game (which refuses it), not redirected")
	check(pcall(file.Delete, "", "DATA") ~= nil, "an empty name is left to the game")
end

-- ---------------------------------------------------------------------------------------------------------------- audit
do
	local fs = fresh()
	seed(fs)
	boot({ mode = "audit", token = "a1" })
	file.Write("cache/index.txt", "v2")
	file.Write("cache/index.txt", "v3")
	file.Write("cache/gm_construct/new.dat", "n")
	file.Append("cache/gm_construct/t1.dat", "x")
	file.Delete("cache/gm_construct/t2.dat", "DATA")
	file.CreateDir("made")
	file.Rename("user.txt", "user2.txt")
	local h = file.Open("opened.txt", "wb", "DATA")
	check(fs.files["cache/index.txt"] == "v3" and not fs.files["cache/gm_construct/t2.dat"], "audit lets the writes and the delete through")
	local log = fs.files["mcp_overlay/a1/audit.txt"]
	check(log ~= nil, "audit writes its log")
	check(log and log:find("W\t1\tcache/index.txt\n", 1, true), "audit: write of an existing file is existed=1")
	local _, n = string.gsub(log or "", "cache/index.txt", "")
	check(n == 1, "audit: a repeated write of the same file is logged once")
	check(log and log:find("W\t0\tcache/gm_construct/new.dat\n", 1, true), "audit: write of a new file is existed=0")
	check(log and log:find("W\t1\tcache/gm_construct/t1.dat\n", 1, true), "audit: append of an existing file")
	check(log and log:find("D\t1\tcache/gm_construct/t2.dat\n", 1, true), "audit: delete")
	check(log and log:find("M\t0\tmade\n", 1, true), "audit: mkdir")
	check(log and log:find("D\t1\tuser.txt\n", 1, true) and log:find("W\t0\tuser2.txt\n", 1, true), "audit: rename is a delete and a write")
	check(log and log:find("W\t0\topened.txt\n", 1, true), "audit: Open for write")
	-- Reads are not logged.
	local len = #log
	file.Read("user2.txt", "DATA") file.Exists("x", "DATA")
	check(#fs.files["mcp_overlay/a1/audit.txt"] == len, "audit: reads are not logged")
	-- The overlay token is ours.
	local st = __mcpagent_data_overlay_state
	check(st.mode == "audit" and st.ops.write >= 3, "status counts the operations")
end

print(string.format("data_overlay_test: %d passed, %d failed", passes, fails))
os.exit(fails == 0 and 0 or 1)
