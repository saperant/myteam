#include "myteam/sandbox.hpp"

#include <functional>
#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <mutex>
#include <stdexcept>

#include "myteam/guard.hpp"
#include "myteam/process.hpp"
#include "myteam/util.hpp"

namespace myteam {

namespace fs = std::filesystem;

bool bwrap_works() {
    static std::once_flag once;
    static bool works = false;
    std::call_once(once, [] {
        auto bwrap = find_executable("bwrap");
        if (!bwrap) return;
        ProcessOptions o;
        o.argv = {bwrap->string(), "--ro-bind", "/", "/", "--dev", "/dev", "--proc", "/proc", "--unshare-all",
                  "--die-with-parent", "true"};
        o.timeout_seconds = 10;
        works = run_process(o, [](std::string_view) {}).exit_code == 0;
    });
    return works;
}

std::vector<std::string> sandbox_argv(const Project& project, const std::string& runner, const fs::path& run_dir) {
    const auto& cfg = project.config();
    if (cfg.sandbox == "none") return {};
    if (!bwrap_works()) {
        if (cfg.sandbox == "bwrap") throw std::runtime_error("sandbox: bwrap is required, but bwrap is not installed or cannot run here");
        return {};
    }
    std::error_code ec;
    const fs::path project_dir = fs::weakly_canonical(project.dir(), ec);
    const fs::path home = std::getenv("HOME") ? fs::path(std::getenv("HOME")) : fs::path("/root");
    std::vector<std::string> a = {find_executable("bwrap")->string(), "--ro-bind", "/", "/", "--dev", "/dev", "--proc",
                                  "/proc", "--tmpfs", "/tmp"};
    if (const char* t = std::getenv("TMPDIR"); t && *t && fs::is_directory(t) && fs::path(t) != "/tmp")
        a.insert(a.end(), {"--tmpfs", t});
    auto bind = [&](const char* how, const fs::path& p) {
        if (fs::exists(p)) a.insert(a.end(), {how, p.string(), p.string()});
    };
    // (Made first: it is under .myteam/runs, which is listed below.)
    fs::create_directories(run_dir, ec);
    // The project, then what in it stays as it is.
    bind("--bind", project_dir);
    // The brief stays the team's to write. Editors and agents' Write tools put
    // a temporary file beside it and rename it over the brief, so its
    // directory must be writable and the brief an ordinary file in it (not a
    // mount): the directories down to it stay writable, everything else in
    // them is mounted read-only, entry by entry. (A new file an agent leaves
    // there is a protected change: the after-run review takes it out.)
    const std::string brief = writable_brief(project);
    const auto globs = protected_globs(project, cfg.guard);
    std::function<void(const fs::path&, const fs::path&)> ro_all_but = [&](const fs::path& dir, const fs::path& keep) {
        std::vector<fs::path> entries;
        for (const auto& e : fs::directory_iterator(dir, ec)) entries.push_back(e.path());
        std::sort(entries.begin(), entries.end());
        for (const fs::path& e : entries) {
            if (e == keep) continue;
            auto rel = keep.lexically_relative(e);
            if (!rel.empty() && *rel.begin() != ".." && fs::is_directory(e, ec) && !fs::is_symlink(e, ec)) {
                ro_all_but(e, keep);
                continue;
            }
            bind("--ro-bind", e);
        }
    };
    for (const fs::path& rel : {fs::path(cfg.plan), fs::path(cfg.changelog), fs::path(".git"), fs::path(".myteam"),
                                fs::path(".github/workflows"), fs::path(".gitlab-ci.yml")}) {
        if (!brief.empty() && starts_with(brief, rel.generic_string() + "/") && fs::is_directory(project_dir / rel, ec))
            ro_all_but(project_dir / rel, project_dir / brief);
        else if (path_protected(globs, rel.generic_string()) ||
                 path_protected(globs, rel.generic_string() + "/x"))  // a directory: what is in it
            bind("--ro-bind", project_dir / rel);
    }
    if (!cfg.backlog.empty()) bind("--ro-bind", project_dir / cfg.backlog);
    for (const auto& r : cfg.readonly) bind("--ro-bind", project_dir / r);
    // The run's own directory: the runner, the MCP server and the guard write there.
    bind("--bind", fs::weakly_canonical(run_dir, ec));
    // The CLI's own state.
    for (const char* p : {".claude", ".claude.json", ".codex", ".vibe", ".cache", ".local/state"}) bind("--bind", home / p);
    // Package managers' download caches (not their credentials, which stay hidden).
    for (const char* p : {".npm", ".cargo/registry", ".cargo/git", "go/pkg/mod", ".gradle/caches", ".m2/repository"})
        bind("--bind", home / p);
    for (const auto& p : cfg.sandbox_writable) bind("--bind", fs::path(p));
    for (const auto& p : project.write_paths()) bind("--bind", fs::path(p));
    // Credentials, hidden.
    for (const char* d : {".ssh", ".aws", ".gnupg", ".config/gh", ".kube", ".docker", ".config/gcloud", ".azure",
                          ".password-store"})
        if (fs::is_directory(home / d)) a.insert(a.end(), {"--tmpfs", (home / d).string()});
    for (const char* f : {".netrc", ".git-credentials", ".pypirc", ".npmrc", ".config/hub"})
        if (fs::is_regular_file(home / f)) a.insert(a.end(), {"--ro-bind", "/dev/null", (home / f).string()});
    // What the operator (or myorg) keeps out of sight: an empty directory in its
    // place. Never the project itself or anything around it.
    for (const std::string& h : cfg.sandbox_hide) {
        fs::path p = fs::path(h).is_absolute() ? fs::path(h) : project_dir / h;
        p = fs::weakly_canonical(p, ec);
        if (!fs::is_directory(p, ec)) continue;
        auto rel = project_dir.lexically_relative(p);
        if (p == project_dir || p == "/" || p == home || (!rel.empty() && *rel.begin() != "..")) continue;
        a.insert(a.end(), {"--tmpfs", p.string()});
    }
    a.insert(a.end(), {"--unshare-all", "--die-with-parent", "--new-session"});
    if (cfg.sandbox_network) a.push_back("--share-net");
    a.insert(a.end(), {"--chdir", project_dir.string(), "--setenv", "MYTEAM_SANDBOX", "bwrap", "--"});
    (void)runner;
    return a;
}

}  // namespace myteam
