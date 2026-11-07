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
    "(channel \"job:<id>\").\n"
    "- job.args: the `args` object of this call. job:elapsed(): seconds since start.\n"
    "- `return value, ...` ends the job: the values are the tool result.\n"
    "A single resume that runs longer than resume_limit_s (default 2 s) without yielding is aborted with an error. "
    "Cancelling the request (or the task) stops the coroutine at the next frame. The job dies with its realm (map change). "
    "Example:\n"
    "local hits = 0\n"
    "for i = 1, 5000 do\n"
    "  local tr = util.TraceLine({ start = Vector(0, 0, 64), endpos = VectorRand() * 4096 })\n"
    "  if tr.Hit then hits = hits + 1 end\n"
    "  if i % 250 == 0 then job:progress(i, 5000, hits .. \" hits\") end\n"
    "  job:step()\n"
    "end\n"
    "return { hits = hits }";

const char *job_protocol() { return kJobProtocol; }

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
    json p{{"code", code}, {"chunkname", chunk}, {"args", args.value("args", json::object())},
           {"slice_ms", args.value("slice_ms", 8.0)}, {"resume_limit_s", args.value("resume_limit_s", 2.0)}};
    json start = agent::rt(realm, "job_start", p);
    if (!start.value("ok", false)) return start;
    std::string id = start["result"]["id"];
    uint64_t gen = agent::generation(realm);
    agent::jobs_wake(realm);
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
        if (state == "done") {
            json out{{"job_id", id}, {"values", s.value("values", json::array())}, {"n", s.value("n", 0)},
                     {"reports", s.value("reports", json::array())}, {"output", s.value("output", "")},
                     {"frames", s["frames"]}, {"resumes", s["resumes"]}, {"cpu_ms", s["cpu_ms"]}, {"elapsed_s", s["elapsed_s"]}};
            if (s.value("reports_dropped", 0) > 0) out["reports_dropped"] = s["reports_dropped"];
            return ok(out);
        }
        if (state == "failed") {
            json e = s.value("error", json::object());
            return agent::err(e.value("code", "lua_error"), e.value("message", "job failed"),
                              json{{"job_id", id}, {"traceback", s.value("traceback", "")}, {"reports", s.value("reports", json::array())},
                                   {"output", s.value("output", "")}, {"frames", s["frames"]}, {"elapsed_s", s["elapsed_s"]}});
        }
        if (state == "cancelled") return agent::err("cancelled", "lua_job cancelled", json{{"job_id", id}});
        if (std::chrono::duration<double>(Clock::now() - t0).count() > timeout) {
            agent::rt(realm, "job_cancel", json{{"id", id}}, 5.0);
            return agent::err("timeout", "lua_job still running after " + std::to_string((int)timeout) + " s: cancelled",
                              json{{"job_id", id}, {"progress", last_progress}, {"reports_count", s.value("reports_count", 0)}});
        }
    }
}

void register_jobs() {
    mcp::register_tool(
        {"lua_job", "Run a long Lua job", kJobProtocol,
         json{{"properties",
               {{"realm", realm_prop("Realm (default server).")},
                {"code", {{"type", "string"}, {"description", "Lua source. Use the `job` object described above."}}},
                {"path", {{"type", "string"},
                          {"description", "Instead of code: file relative to lua/mcpagent_tmp/ or absolute (errors then point to the file)."}}},
                {"args", {{"type", "object"}, {"description", "Arbitrary data exposed to the script as job.args."}}},
                {"timeout_s", {{"type", "number"}, {"minimum", 1}, {"maximum", 3600},
                               {"description", "Cancel the job after this many seconds (default 300). 15 s or more: runs as an MCP task when the client supports it."}}},
                {"slice_ms", {{"type", "number"}, {"minimum", 0.5}, {"maximum", 100}, {"description", "Time per frame before job:step() yields (default 8)."}}},
                {"resume_limit_s", {{"type", "number"}, {"minimum", 0.1}, {"maximum", 30},
                                    {"description", "Abort if a single resume runs this long without yielding (default 2)."}}}}},
              {"oneOf", json::array({json{{"required", {"code"}}}, json{{"required", {"path"}}}})}},
         json{{"destructiveHint", true}}, lua_job_tool});
    mcp::set_task_support("lua_job", "optional");
}

}  // namespace tools
