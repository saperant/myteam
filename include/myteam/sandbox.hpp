// Layer 3 of the safeguards: the agent's process in a sandbox.
//
// With bubblewrap (`sandbox: auto`, the default, when bwrap is installed and
// works; `bwrap` to require it; `none` to turn it off) every agent CLI, of
// every runner, runs with:
//
//   - the whole system read-only, a private /tmp;
//   - the project writable, except the protected paths, read-only: the plan,
//     the backlog, the changelog, .git/, .myteam/ (but the project brief, the
//     team's to write, and the run's own directory, where the runner, the MCP
//     server and the guard write), CI;
//   - the CLI's own state writable (~/.claude, ~/.claude.json, ~/.codex,
//     ~/.vibe, ~/.cache), and `sandbox_writable:` paths;
//   - credentials hidden (~/.ssh, ~/.aws, ~/.gnupg, ~/.config/gh, ...);
//   - the network (the CLIs need their APIs; `sandbox_network: false` cuts it).
//
// So no command or script of the agent, whatever it is, can change the
// system, the user's home, the plan or myteam's state, or read credentials.
#pragma once

#include <filesystem>
#include <string>
#include <vector>

#include "myteam/project.hpp"

namespace myteam {

/// bwrap is installed and can create a sandbox here.
bool bwrap_works();

/// The argv prefix that runs a CLI of `runner` in the sandbox for a run in
/// `run_dir` (empty when the project's sandbox setting turns it off or it is
/// unavailable with `auto`). Throws when `bwrap` is required but unusable.
std::vector<std::string> sandbox_argv(const Project& project, const std::string& runner,
                                      const std::filesystem::path& run_dir);

}  // namespace myteam
