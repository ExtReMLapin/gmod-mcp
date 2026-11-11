// Build-time tool: prints the offline snapshot of the server (tools, prompts, resources, instructions, capabilities)
// built from the module's own sources, without the game. CMake embeds it into the bridge, which serves it while the
// game has never answered (fresh install, a registry's Docker build): the full tool list, calls then saying the game is
// not running. Not shipped.
#include <cstdio>

#include "agent.h"
#include "mcp.h"
#include "resources.h"
#include "tools.h"

int main(int argc, char **argv) {
    auto &cfg = agent::config();
    // Neutral values for the machine-specific parts of the instructions.
    cfg.mode = agent::Mode::Client;
    cfg.instance_id = "none yet: the game is not running";
    cfg.tmp_dir = "<game folder>/garrysmod/lua/mcpagent_tmp";
    cfg.game_dir = "<game folder>/garrysmod";
    tools::register_all();
    mcp::set_instructions(resources::instructions());
    std::string s = mcp::offline_snapshot().dump(-1, ' ', false, nlohmann::json::error_handler_t::replace);
    FILE *f = argc > 1 ? std::fopen(argv[1], "wb") : stdout;  // usage: gmod-mcp-snapshot [out.json]
    if (!f) return 1;
    std::fwrite(s.data(), 1, s.size(), f);
    return f == stdout ? 0 : (std::fclose(f) == 0 ? 0 : 1);
}
