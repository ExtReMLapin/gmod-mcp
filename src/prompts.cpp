#include "prompts.h"

#include <algorithm>

#include "agent.h"
#include "platform/platform.h"
#include "realms.h"
#include "tools.h"
#include "util.h"

using nlohmann::json;

namespace prompts {

struct Arg {
    const char *name, *title, *description;
    bool required;
};

struct Prompt {
    const char *name, *title, *description;
    std::vector<Arg> args;
};

static const std::vector<Prompt> &all() {
    static const std::vector<Prompt> p = {
        {"write-lua-job", "Write a long-running Lua job",
         "Write and run a lua_job script for a long task (scan, batch of traces, timed test) with progress reporting.",
         {{"goal", "Goal", "What the job must do.", true}, {"realm", "Realm", "menu, client or server (default server).", false}}},
        {"debug-errors", "Debug recent Lua errors",
         "Collect the recent Lua errors, find their root cause in the code and propose (or apply) a fix.",
         {{"realm", "Realm", "Only this realm (menu, client or server).", false}}},
        {"run-tests", "Run a Lua test file", "Run a GLuaTest-format test file, then analyze the failures.",
         {{"path", "Test file", "Path relative to lua/mcpagent_tmp/ (or absolute).", true},
          {"realm", "Realm", "Realm to run in (default server).", false}}},
        {"check-map", "Load and check a map", "Load a map, then report errors, entity counts, performance and a screenshot.",
         {{"map", "Map", "Map name without .bsp (e.g. gm_construct).", true}}},
    };
    return p;
}

json list() {
    json arr = json::array();
    for (const auto &p : all()) {
        json args = json::array();
        for (const auto &a : p.args)
            args.push_back(json{{"name", a.name}, {"title", a.title}, {"description", a.description}, {"required", a.required}});
        arr.push_back(json{{"name", p.name}, {"title", p.title}, {"description", p.description}, {"arguments", args}});
    }
    return arr;
}

static std::string arg(const json &args, const char *name, const std::string &def = "") {
    if (args.is_object() && args.contains(name) && args[name].is_string() && !args[name].get<std::string>().empty())
        return args[name].get<std::string>();
    return def;
}

static std::string body(const std::string &name, const json &a) {
    if (name == "write-lua-job") {
        std::string realm = arg(a, "realm", "server");
        return "Write a Lua script for the gmod-mcp `lua_job` tool, then run it in the " + realm +
               " realm with lua_job.\n\nGoal: " + arg(a, "goal") +
               "\n\nThe script runs as a coroutine resumed once per frame and must follow this protocol:\n\n" + tools::job_protocol() +
               "\n\nGuidelines:\n"
               "- Check the API you use with api_search (it lists what really exists in each realm) before writing the script.\n"
               "- Call job:progress() regularly (at least every second of work) with done/total when the amount of work is known.\n"
               "- Call job:step() inside every loop that can run more than a few hundred iterations.\n"
               "- Use job:report() for intermediate findings worth keeping even if the job fails later.\n"
               "- Return a compact summary table; put bulky data in reports.\n"
               "- For anything longer than ~15 s pass timeout_s so the client can run it as a background task.\n"
               "- Save the script with lua_file_write (or directly in the Lua working directory) and run it with `path`, so errors point to the file.";
    }
    if (name == "debug-errors") {
        std::string realm = arg(a, "realm");
        return "Debug the recent Lua errors of the running Garry's Mod instance" + (realm.empty() ? "" : " (realm " + realm + ")") +
               ".\n\n1. Call errors_since with cursor 0 to list them (grouped by message; note counts and realms).\n"
               "2. For each distinct error, read the stack, open the source file (lua_file_read for the temporary directory, or the "
               "addon file on disk) and find the root cause.\n"
               "3. Check any API you suspect with api_search / wiki_page.\n"
               "4. Propose a fix; when the file belongs to the agent's working directory, apply it and re-run to confirm with "
               "errors_since (use the cursor returned earlier).\n"
               "Summarize: error, cause, fix, verified or not.";
    }
    if (name == "run-tests") {
        return "Run the Lua tests in `" + arg(a, "path") + "` with tests_run (realm " + arg(a, "realm", "server") +
               ").\nIf the realm is unavailable, load a map first (map_load gm_construct). For every failing case, read the test and the "
               "code under test, explain the failure and propose a fix. Format reference: resource mcpagent://tests-format.";
    }
    if (name == "check-map") {
        return "Load the map `" + arg(a, "map") +
               "` with map_load, then check it:\n"
               "1. errors_since (cursor from before the load) and console_read for warnings.\n"
               "2. status and perf_snapshot (fps, frame times, entity count).\n"
               "3. ents_find to summarize the entity classes present.\n"
               "4. screenshot to show the spawn view.\n"
               "Report anything abnormal.";
    }
    return "";
}

json get(const std::string &name, const json &args, std::string &err) {
    for (const auto &p : all()) {
        if (name != p.name) continue;
        for (const auto &a : p.args)
            if (a.required && arg(args, a.name).empty()) {
                err = std::string("missing required argument: ") + a.name;
                return nullptr;
            }
        json text{{"type", "text"}, {"text", body(name, args)}};
        return json{{"description", p.description}, {"messages", json::array({json{{"role", "user"}, {"content", text}}})}};
    }
    err = "unknown prompt: " + name;
    return nullptr;
}

std::vector<std::string> complete(const std::string &prompt, const std::string &argname, const std::string &value) {
    std::vector<std::string> out;
    std::string v = util::to_lower(value);
    auto starts = [&](const std::string &x) { return util::to_lower(x).rfind(v, 0) == 0; };
    if (argname == "realm") {
        for (const char *r : {"menu", "client", "server"})
            if (starts(r)) out.push_back(r);
    } else if (argname == "path" && prompt == "run-tests") {
        for (const auto &f : plat::list_files(agent::config().tmp_dir, true))
            if (starts(f) && f.size() > 4 && f.compare(f.size() - 4, 4, ".lua") == 0) out.push_back(f);
        std::sort(out.begin(), out.end());
    } else if (argname == "map" && prompt == "check-map") {
        int host = agent::config().mode == agent::Mode::Client ? realms::MENU : realms::SERVER;
        json r = agent::rt(host, "map_list", json{{"filter", value}, {"limit", 200}}, 5.0);
        if (r.value("ok", false))
            for (const auto &m : r["result"].value("maps", json::array()))
                if (m.is_string()) out.push_back(m);
        std::stable_sort(out.begin(), out.end(), [&](const std::string &x, const std::string &y) { return starts(x) && !starts(y); });
    }
    return out;
}

}  // namespace prompts
