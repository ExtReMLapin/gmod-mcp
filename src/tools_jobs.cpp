// Tool: lua_job (long Lua work spread over frames, with progress, intermediate reports and cancellation).
#include <chrono>

#include "agent.h"
#include "mcp.h"
#include "platform/platform.h"
#include "realms.h"
#include "reqctx.h"
#include "tools.h"

namespace tools {

using Clock = std::chrono::steady_clock;

static const char *kJobProtocol =
    "Runs a Lua script as a coroutine resumed once per frame, so long work (scans, batches of traces, multi-second tests) "
    "does not freeze the game and reports progress while it runs. The script MUST cooperate through the `job` object "
    "(global inside the chunk, also its first argument):\n"
    "- job:progress(done, total, message) or job:progress(message): forwarded to the client as MCP progress "
    "(and as the task status when run as a task).\n"
    "- job:step(): call it inside long loops; it yields only when this frame's time slice (slice_ms, default 8 ms) is used up.\n"
    "- job:yield() / job:frames(n) / job:sleep(seconds) / job:wait(fn, timeout): resume on a later frame.\n"
    "- job:report(value [, channel]): intermediate result, returned at the end and readable live with reports_read "
    "(channel \"job:<id>\"). Each report is bounded: depth 8, 5000 entries per table, strings of 1 MiB (an item the "
    "serializer cannot take, over 20000 values, becomes a {__type=\"report_too_large\"} marker); a cut part is marked "
    "truncated and counted in reports_truncated. At most 1000 reports are kept (more are counted in reports_dropped), "
    "32 MiB of strings in all: put big data in the returned values or a file instead.\n"
    "- args (a global, same table as job.args): the `args` object of this call. job:elapsed(): seconds since start.\n"
    "- `return value, ...` ends the job: the values are the tool result.\n"
    "Realm: server by default (a map must be loaded); menu always exists in client mode. The code is `code`, or `path` "
    "(a file, found like lua_exec_file finds it).\n"
    "By default the call waits for the end and returns {job_id, values, n, reports, output (printed text), frames, "
    "resumes, cpu_ms, elapsed_s, generation, truncated?, concurrent_jobs?}; timeout_s (default 300) cancels a job that "
    "is still running. Errors: lua_syntax_error, lua_error (with traceback, reports and output so far), realm_reset (the "
    "map changed: the job died with its Lua state), cancelled, timeout.\n"
    "Globals the script assigns go to the realm's _G (they outlive the job and the next job sees them) unless "
    "fresh_env: true.\n"
    "async: true starts the job and returns {job_id, state, realm, generation} at once: poll it with lua_job_status "
    "(wait_s to wait), stop it with lua_job_cancel; two jobs can then run side by side, e.g. one per realm. A job started "
    "while another runs in the same realm is reported in concurrent_jobs: their timings and state mix.\n"
    "Returned values come back whole (no cut unless depth/limit/max_string; then the result says truncated).\n"
    "The LuaJIT compiler stays on (timings are real). A resume that never yields freezes the game: polling it then fails "
    "with main_thread_timeout (about 10 s) and status shows main_thread_stalled. resume_limit_s (optional, best effort) "
    "aborts a resume that runs that long without yielding, for loops written in the script itself. "
    "Cancelling the request (or the task) stops the coroutine at the next frame. The job dies with its realm (map change). "
    "Example:\n"
    "local hits = 0\n"
    "for i = 1, 5000 do\n"
    "  local tr = util.TraceLine({ start = Vector(0, 0, 64), endpos = VectorRand() * 4096 })\n"
    "  if tr.Hit then hits = hits + 1 end\n"
    "  if i % 250 == 0 then job:progress(i, 5000, hits .. \" hits\") end\n"
    "  job:step()\n"
    "end\n"
    "return { hits = hits }\n"
    "called as {\"realm\": \"server\", \"code\": \"<the script above>\", \"timeout_s\": 120}.";

const char *job_protocol() { return kJobProtocol; }

// Final result of a finished job (from job_poll), shared by lua_job and lua_job_status.
static json job_result(const json &s, const json &args) {
    std::string state = s.value("state", "running");
    std::string id = s.value("id", "");
    if (state == "done") {
        json out{{"job_id", id}, {"values", s.value("values", json::array())}, {"n", s.value("n", 0)},
                 {"reports", s.value("reports", json::array())}, {"output", s.value("output", "")},
                 {"frames", s["frames"]}, {"resumes", s["resumes"]}, {"cpu_ms", s["cpu_ms"]}, {"elapsed_s", s["elapsed_s"]},
                 {"generation", s.value("generation", json())}};
        if (s.value("reports_dropped", 0) > 0) out["reports_dropped"] = s["reports_dropped"];
        if (s.value("reports_truncated", 0) > 0) out["reports_truncated"] = s["reports_truncated"];
        if (s.contains("truncated")) {
            out["truncated"] = s["truncated"];
            out["truncated_note"] = s.value("truncated_note", "");
        }
        return project_values(ok(out), args);
    }
    if (state == "failed") {
        json e = s.value("error", json::object());
        return agent::err(e.value("code", "lua_error"), e.value("message", "job failed"),
                          json{{"job_id", id}, {"traceback", s.value("traceback", "")}, {"reports", s.value("reports", json::array())},
                               {"output", s.value("output", "")}, {"frames", s["frames"]}, {"elapsed_s", s["elapsed_s"]}});
    }
    return agent::err("cancelled", "lua_job cancelled", json{{"job_id", id}});
}

// "server-g3-12" -> server realm, generation 3 (the old "server-12" form has no generation).
JobId parse_job_id(const std::string &id) {
    JobId out;
    size_t dash = id.find('-');
    if (dash == std::string::npos) return out;
    int realm = realms::parse(id.substr(0, dash));
    if (realm < 0) return out;
    out.realm = realm;
    // "g<digits>-<n>"
    size_t p = dash + 1;
    if (p < id.size() && id[p] == 'g') {
        size_t q = p + 1, end = q;
        while (end < id.size() && id[end] >= '0' && id[end] <= '9') ++end;
        if (end > q && end < id.size() && id[end] == '-') {
            if (end - q > 18) return JobId{};  // not a number a Lua state could have produced
            out.has_generation = true;
            out.generation = std::stoull(id.substr(q, end - q));
        }
    }
    return out;
}

std::string job_reset_message(const JobId &id, uint64_t current_generation) {
    if (id.realm < 0 || !id.has_generation || id.generation == current_generation) return "";
    return std::string("the ") + realms::name(id.realm) + " realm was re-created (map change) since this job started: the job is gone";
}

// An id from an earlier Lua state: realm_reset, without touching the new state's jobs (whose ids restart at 1).
static json job_reset_error(const std::string &id, const JobId &jid) {
    std::string why = job_reset_message(jid, agent::generation(jid.realm));
    if (why.empty()) return json();
    return agent::err("realm_reset", why, json{{"job_id", id}});
}

static json lua_job_tool(const json &args) {
    int realm = realm_of(args, "realm", realms::SERVER);
    if (realm < 0) return agent::err("invalid_params", "unknown realm");
    std::string code, chunk;
    if (args.contains("path")) {
        std::string abs, e;
        if (!resolve_lua_file(args["path"], abs, chunk, e)) return agent::err("not_found", e);
        if (!plat::read_file(abs, code)) return agent::err("io_error", "cannot read: " + abs);
    } else if (args.contains("code")) {
        code = args["code"];
        chunk = "=mcpagent:job:" + std::string(realms::name(realm));
    } else {
        return agent::err("invalid_params", "pass `code` or `path`");
    }
    bool async = args.value("async", false);
    json p{{"code", code}, {"chunkname", args.value("chunkname", chunk)}, {"args", args.value("args", json::object())},
           {"slice_ms", args.value("slice_ms", 8.0)}, {"fresh_env", args.value("fresh_env", false)}, {"async", async}};
    for (const char *k : {"resume_limit_s", "jit", "depth", "limit", "max_string"})
        if (args.contains(k)) p[k] = args[k];
    json start = agent::rt(realm, "job_start", p);
    if (!start.value("ok", false)) return start;
    std::string id = start["result"]["id"];
    json concurrent = start["result"].value("other_jobs", json());
    uint64_t gen = agent::generation(realm);
    agent::jobs_wake(realm);
    if (async) {
        json out{{"job_id", id}, {"state", "running"}, {"realm", realms::name(realm)}, {"generation", start["result"].value("generation", json())},
                 {"next", "lua_job_status {id: \"" + id + "\"} polls it (wait_s to wait for the end) and returns the result once "
                          "finished; lua_job_cancel stops it. An uncollected result is dropped after 1 h; the job dies with its "
                          "realm (map change)."}};
        if (!concurrent.is_null()) out["concurrent_jobs"] = concurrent;
        return ok(out);
    }
    double timeout = args.value("timeout_s", 300.0);
    auto t0 = Clock::now();
    int64_t last_seq = 0;
    json last_progress;
    reqctx::progress(0, 0, "job " + id + " started");
    for (;;) {
        plat::sleep_ms(100);
        if (reqctx::cancelled()) {
            agent::rt(realm, "job_cancel", json{{"id", id}}, 5.0);
            return agent::err("cancelled", "lua_job cancelled", json{{"job_id", id}, {"progress", last_progress}});
        }
        if (agent::generation(realm) != gen || !agent::realm_present(realm))
            return agent::err("realm_reset", std::string("the ") + realms::name(realm) + " realm was destroyed while the job was running (map change?)",
                              json{{"job_id", id}, {"progress", last_progress}});
        json st = agent::rt(realm, "job_poll", json{{"id", id}});
        if (!st.value("ok", false)) return st;
        json s = st["result"];
        if (s.contains("progress") && s["progress"].is_object()) {
            const json &pr = s["progress"];
            int64_t seq = pr.value("seq", (int64_t)0);
            if (seq != last_seq) {
                last_seq = seq;
                last_progress = pr;
                double done = pr.contains("done") && pr["done"].is_number() ? pr["done"].get<double>() : (double)seq;
                double total = pr.contains("total") && pr["total"].is_number() ? pr["total"].get<double>() : 0.0;
                reqctx::progress(done, total, pr.value("message", ""));
            }
        }
        std::string state = s.value("state", "running");
        if (state != "running") {
            json r = job_result(s, args);
            if (!concurrent.is_null() && r.value("ok", false)) r["result"]["concurrent_jobs"] = concurrent;
            return r;
        }
        if (std::chrono::duration<double>(Clock::now() - t0).count() > timeout) {
            agent::rt(realm, "job_cancel", json{{"id", id}}, 5.0);
            return agent::err("timeout", "lua_job still running after " + std::to_string((int)timeout) + " s: cancelled",
                              json{{"job_id", id}, {"progress", last_progress}, {"reports_count", s.value("reports_count", 0)}});
        }
    }
}

// lua_job_status: one job's state (its result once finished, which also forgets it), or every job of the realms.
static json lua_job_status_tool(const json &args) {
    if (!args.contains("id")) {
        json all = json::array();
        for (int r : {realms::MENU, realms::CLIENT, realms::SERVER}) {
            if (!agent::realm_present(r)) continue;
            json l = agent::rt(r, "job_list", json::object(), 5.0);
            if (!l.value("ok", false)) continue;
            for (auto j : l["result"].value("jobs", json::array())) {
                j["realm"] = realms::name(r);
                all.push_back(j);
            }
        }
        return ok(json{{"jobs", all}});
    }
    std::string id = args["id"];
    JobId jid = parse_job_id(id);
    int realm = jid.realm;
    if (realm < 0) return agent::err("invalid_params", "id: expected a job id like 'server-g3-1'");
    double wait_s = args.value("wait_s", 0.0);
    auto t0 = Clock::now();
    for (;;) {
        json reset = job_reset_error(id, jid);
        if (!reset.is_null()) return reset;
        json st = agent::rt(realm, "job_poll", json{{"id", id}, {"peek", true}});
        if (!st.value("ok", false)) return st;
        const json &s = st["result"];
        if (s.value("state", "running") != "running") {
            json fin = agent::rt(realm, "job_poll", json{{"id", id}});
            if (!fin.value("ok", false)) return fin;
            return job_result(fin["result"], args);
        }
        if (std::chrono::duration<double>(Clock::now() - t0).count() >= wait_s || reqctx::cancelled()) {
            return ok(json{{"job_id", id}, {"state", "running"}, {"frames", s["frames"]}, {"elapsed_s", s["elapsed_s"]},
                           {"cpu_ms", s.value("cpu_ms", json())}, {"progress", s.value("progress", json())},
                           {"reports_count", s.value("reports_count", 0)}, {"chunkname", s.value("chunkname", json())}});
        }
        plat::sleep_ms(100);
    }
}

static json lua_job_cancel_tool(const json &args) {
    std::string id = args["id"];
    JobId jid = parse_job_id(id);
    int realm = jid.realm;
    if (realm < 0) return agent::err("invalid_params", "id: expected a job id like 'server-g3-1'");
    json reset = job_reset_error(id, jid);
    if (!reset.is_null()) return reset;
    json r = agent::rt(realm, "job_cancel", json{{"id", id}}, 5.0);
    if (!r.value("ok", false)) return r;
    return ok(json{{"job_id", id}, {"cancelled", r["result"].value("cancelled", false)},
                   {"note", "the job stops at its next frame; its final state stays readable with lua_job_status"}});
}

void register_jobs() {
    mcp::register_tool(
        {"lua_job", "Run a long Lua job", kJobProtocol,
         json{{"properties",
               {{"realm", realm_prop("Realm (default server).")},
                {"code", {{"type", "string"}, {"description", "Lua source. Use the `job` object described above."}}},
                {"path", {{"type", "string"},
                          {"description", "Instead of code: a file, absolute (e.g. in your git worktree) or relative (lua/mcpagent_tmp/, "
                                          "the mounted Lua folder if any, then the game folder, then lua/, gamemodes/, addons/*/lua/). Errors then point to the file. "
                                          "Prefer it to pasting a file's content into code."}}},
                {"fields", {{"type", "array"}, {"items", {{"type", "string"}}}, {"maxItems", 50},
                            {"description", "Keep only these keys of each returned table (e.g. [\"pass\", \"fails\"])."}}},
                {"args", {{"type", "object"}, {"description", "Arbitrary data exposed to the script as job.args."}}},
                {"timeout_s", {{"type", "number"}, {"minimum", 1}, {"maximum", 3600},
                               {"description", "Cancel the job after this many seconds (default 300). 15 s or more: runs as an MCP task when the client supports it."}}},
                {"slice_ms", {{"type", "number"}, {"minimum", 0.5}, {"maximum", 100}, {"description", "Time per frame before job:step() yields (default 8)."}}},
                {"resume_limit_s", {{"type", "number"}, {"minimum", 0.1}, {"maximum", 600},
                                    {"description", "Optional, best effort: abort a resume that runs this long without yielding "
                                                    "(loops written in the script itself). None by default."}}},
                {"jit", {{"type", "boolean"},
                         {"description", "Obsolete: the compiler is never turned off any more. jit: true now only ignores resume_limit_s."}}},
                {"async", {{"type", "boolean"},
                           {"description", "true: start the job and return its id at once (poll with lua_job_status). Default false."}}},
                {"fresh_env", {{"type", "boolean"},
                               {"description", "Keep the globals the script assigns in its own table instead of _G (no leak "
                                               "into later jobs; reads still see _G). Default false."}}},
                {"chunkname", {{"type", "string"}, {"maxLength", 260},
                               {"description", "Chunk name shown by debug.getinfo and tracebacks (default: the file's path or "
                                               "'=mcpagent:job:<realm>')."}}},
                {"depth", {{"type", "integer"}, {"minimum", 0}, {"maximum", 200}, {"description", "Cut returned tables deeper than this (default: no cut)."}}},
                {"limit", {{"type", "integer"}, {"minimum", 1}, {"maximum", 10000000}, {"description", "Cut returned tables longer than this (default: no cut)."}}},
                {"max_string", {{"type", "integer"}, {"minimum", 16}, {"maximum", 1073741824},
                                {"description", "Cut returned strings longer than this (default: no cut)."}}}}},
              {"oneOf", json::array({json{{"required", {"code"}}}, json{{"required", {"path"}}}})}},
         json{{"destructiveHint", true}}, lua_job_tool});
    mcp::set_task_support("lua_job", "optional");
    mcp::register_tool(
        {"lua_job_status", "State of a Lua job",
         "With id: the state of a job started by lua_job (frames, elapsed_s, cpu_ms, last progress, reports_count); once it "
         "has finished, its full result exactly like lua_job's, and the job is then forgotten. wait_s waits up to that long "
         "for the end. A job id belongs to one Lua state: after a map change (new realm) an id of the old state answers realm_reset. Without id: every job of every realm, running or finished but not collected yet.",
         json{{"properties",
               {{"id", {{"type", "string"}, {"description", "Job id returned by lua_job (e.g. 'server-g3-1')."}}},
                {"wait_s", {{"type", "number"}, {"minimum", 0}, {"maximum", 600}, {"description", "Wait up to this long for the job to finish (default 0)."}}},
                {"fields", {{"type", "array"}, {"items", {{"type", "string"}}}, {"maxItems", 50},
                            {"description", "Keep only these keys of each returned table."}}}}}},
         json{{"readOnlyHint", false}}, lua_job_status_tool});
    mcp::register_tool({"lua_job_cancel", "Cancel a Lua job",
                        "Asks a running job (started by lua_job, e.g. with async: true) to stop: it ends at its next frame, before "
                        "resuming. Result: {job_id, cancelled (false if the id is unknown), note}. Its final state (cancelled) "
                        "stays readable once with lua_job_status. Example: {\"id\": \"server-g3-1\"}.",
                        json{{"properties", {{"id", {{"type", "string"}, {"description", "Job id (e.g. 'server-g3-1')."}}}}}, {"required", {"id"}}},
                        json{{"destructiveHint", true}}, lua_job_cancel_tool});
}

}  // namespace tools
