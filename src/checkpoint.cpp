#include "myteam/checkpoint.hpp"

#include <algorithm>
#include <filesystem>
#include <regex>
#include <set>
#include <unistd.h>

#include <taskml/json.hpp>

#include "myteam/guard.hpp"
#include "myteam/plan.hpp"
#include "myteam/process.hpp"
#include "myteam/sandbox.hpp"
#include "myteam/util.hpp"

namespace myteam {

namespace fs = std::filesystem;
using taskml::Json;

namespace {

struct GitOut {
    int code = -1;
    std::vector<std::string> lines;
    std::string error;
};

// git_dir empty: the project's own repository; otherwise myteam's private
// one (.myteam/checkpoints.git) with the project as its work tree.
GitOut git(const Project& p, const std::string& git_dir, std::vector<std::string> args,
           std::map<std::string, std::string> env = {}) {
    ProcessOptions o;
    o.argv = {"git", "-c", "core.quotepath=off"};
    if (!git_dir.empty()) {
        o.argv.insert(o.argv.end(), {"-c", "core.bare=false"});
        env["GIT_DIR"] = git_dir;
        env["GIT_WORK_TREE"] = p.dir().string();
    }
    o.argv.insert(o.argv.end(), args.begin(), args.end());
    o.cwd = p.dir();
    o.env = std::move(env);
    o.env.try_emplace("GIT_AUTHOR_NAME", "myteam");
    o.env.try_emplace("GIT_AUTHOR_EMAIL", "myteam@localhost");
    o.env.try_emplace("GIT_COMMITTER_NAME", "myteam");
    o.env.try_emplace("GIT_COMMITTER_EMAIL", "myteam@localhost");
    o.env["GIT_TERMINAL_PROMPT"] = "0";
    o.timeout_seconds = 120;
    GitOut out;
    ProcessResult r = run_process(o, [&](std::string_view l) { out.lines.emplace_back(l); });
    out.code = r.exit_code;
    out.error = r.error;
    return out;
}

std::string one(const GitOut& g) { return g.lines.empty() ? "" : std::string(trim(g.lines[0])); }

}  // namespace

bool is_git_repo(const Project& project) {
    GitOut g = git(project, "", {"rev-parse", "--show-toplevel"});
    if (g.code != 0 || g.lines.empty()) return false;
    std::error_code ec;
    return fs::equivalent(fs::path(one(g)), project.dir(), ec);
}

std::string checkpoint_repo(const Project& project) {
    if (is_git_repo(project)) return "";
    const fs::path dir = project.state_dir() / "checkpoints.git";
    if (!fs::exists(dir / "HEAD")) {
        fs::create_directories(dir);
        ProcessOptions o;
        o.argv = {"git", "init", "-q", "--bare", dir.string()};
        o.cwd = project.dir();
        if (run_process(o, [](std::string_view) {}).exit_code != 0) return "";
        fs::create_directories(dir / "info");
        write_file_atomic(dir / "info" / "exclude",
                          "# myteam's checkpoints of a project without git: what is not the work\n"
                          "/.myteam/\n.venv/\nvenv/\nnode_modules/\n__pycache__/\n*.pyc\n.pytest_cache/\n"
                          ".mypy_cache/\n.tox/\n/build/\n/dist/\n/target/\n.cache/\n");
    }
    return dir.string();
}

std::optional<Snapshot> take_snapshot(const Project& project, const std::string& message, std::string* error) {
    const std::string gd = checkpoint_repo(project);
    if (!is_git_repo(project) && gd.empty()) {
        if (error) *error = "git is not available";
        return std::nullopt;
    }
    const fs::path index = project.state_dir() / "checkpoint.index";
    fs::create_directories(project.state_dir());
    std::error_code ec;
    fs::remove(index, ec);
    std::map<std::string, std::string> env{{"GIT_INDEX_FILE", index.string()}};
    auto fail = [&](const std::string& what, const GitOut& g) -> std::optional<Snapshot> {
        if (error) *error = what + (g.error.empty() ? "" : ": " + g.error);
        return std::nullopt;
    };
    GitOut add = git(project, gd, {"add", "-A", "--", "."}, env);
    if (add.code != 0) return fail("git add failed", add);
    // myteam's own files are not the run's work. (Not as exclude pathspecs:
    // git refuses those when the paths are ignored.)
    git(project, gd, {"rm", "-r", "-q", "--cached", "--ignore-unmatch", "--", ".myteam/runs", ".myteam/checkpoint.index",
                  ".myteam/hugo.log"},
        env);
    GitOut tree = git(project, gd, {"write-tree"}, env);
    fs::remove(index, ec);
    if (tree.code != 0 || one(tree).empty()) return fail("git write-tree failed", tree);
    Snapshot s;
    s.git_dir = gd;
    s.tree = one(tree);
    std::vector<std::string> args = {"commit-tree", s.tree, "-m", message};
    GitOut head = git(project, gd, {"rev-parse", "-q", "--verify", "HEAD"});
    if (head.code == 0 && !one(head).empty()) args.insert(args.begin() + 2, {"-p", one(head)});
    GitOut commit = git(project, gd, args);
    if (commit.code != 0 || one(commit).empty()) return fail("git commit-tree failed", commit);
    s.commit = one(commit);
    // A ref keeps it from garbage collection.
    git(project, gd, {"update-ref", "refs/myteam/checkpoints/" + file_timestamp() + "-" + s.commit.substr(0, 8), s.commit});
    return s;
}

std::vector<FileChange> diff_snapshots(const Project& project, const Snapshot& a, const Snapshot& b) {
    std::vector<FileChange> out;
    GitOut ns = git(project, a.git_dir, {"diff", "--name-status", "-M", a.tree, b.tree});
    for (const auto& l : ns.lines) {
        auto parts = std::vector<std::string>{};
        std::size_t pos = 0;
        while (pos <= l.size()) {
            std::size_t t = l.find('\t', pos);
            if (t == std::string::npos) t = l.size();
            parts.push_back(l.substr(pos, t - pos));
            pos = t + 1;
        }
        if (parts.size() < 2 || parts[0].empty()) continue;
        FileChange c;
        c.status = parts[0][0];
        if ((c.status == 'R' || c.status == 'C') && parts.size() >= 3) {
            c.old_path = parts[1];
            c.path = parts[2];
        } else {
            c.path = parts[1];
        }
        out.push_back(std::move(c));
    }
    GitOut num = git(project, a.git_dir, {"diff", "--numstat", "-M", a.tree, b.tree});
    for (const auto& l : num.lines) {
        std::size_t t1 = l.find('\t'), t2 = l.find('\t', t1 + 1);
        if (t1 == std::string::npos || t2 == std::string::npos) continue;
        std::string add = l.substr(0, t1), del = l.substr(t1 + 1, t2 - t1 - 1), path = l.substr(t2 + 1);
        // Renames show as "old => new" or "dir/{old => new}".
        for (FileChange& c : out) {
            bool same = c.path == path || (!c.old_path.empty() && path.find(" => ") != std::string::npos &&
                                           path.find(fs::path(c.path).filename().string()) != std::string::npos);
            if (!same) continue;
            if (add == "-") c.binary = true;
            else {
                c.added = std::atoi(add.c_str());
                c.removed = std::atoi(del.c_str());
            }
            break;
        }
    }
    return out;
}

bool is_test_path(std::string_view path) {
    std::string p = lower(path);
    std::string name = fs::path(p).filename().string();
    if (p.find("/tests/") != std::string::npos || starts_with(p, "tests/") || p.find("/test/") != std::string::npos ||
        starts_with(p, "test/") || p.find("__tests__/") != std::string::npos || p.find("/spec/") != std::string::npos)
        return true;
    if (starts_with(name, "test_") || starts_with(name, "test-")) return true;
    for (const char* s : {"_test.", ".test.", ".spec.", "_spec.", "tests."})
        if (name.find(s) != std::string::npos) return true;
    return false;
}

namespace {

std::vector<std::string> added_lines(const Project& p, const Snapshot& a, const Snapshot& b, const std::string& path) {
    std::vector<std::string> out;
    GitOut d = git(p, a.git_dir, {"diff", "-U0", "-M", a.tree, b.tree, "--", path});
    for (const auto& l : d.lines)
        if (starts_with(l, "+") && !starts_with(l, "+++")) out.push_back(l.substr(1));
    return out;
}

const std::vector<std::pair<std::regex, const char*>>& secret_patterns() {
    static const std::vector<std::pair<std::regex, const char*>> p = {
        {std::regex("-----BEGIN [A-Z ]*PRIVATE KEY-----"), "a private key"},
        {std::regex("AKIA[0-9A-Z]{16}"), "an AWS access key"},
        {std::regex("gh[pousr]_[A-Za-z0-9]{36}"), "a GitHub token"},
        {std::regex("github_pat_[A-Za-z0-9_]{60,}"), "a GitHub token"},
        {std::regex("sk-ant-[A-Za-z0-9_-]{20,}"), "an Anthropic API key"},
        {std::regex("sk-(proj-)?[A-Za-z0-9]{32,}"), "an OpenAI API key"},
        {std::regex("xox[baprs]-[A-Za-z0-9-]{10,}"), "a Slack token"},
        {std::regex("AIza[0-9A-Za-z_-]{35}"), "a Google API key"},
    };
    return p;
}

const std::regex& skip_pattern() {
    static const std::regex r(
        R"((@pytest\.mark\.(skip|xfail)|pytest\.skip\(|@unittest\.skip|\bxit\(|\bxdescribe\(|\b(it|describe|test)\.skip\(|GTEST_SKIP|\bDISABLED_|#\[ignore\]|t\.Skip\())");
    return r;
}

}  // namespace

bool is_bytecode(std::string_view rel) {
    auto ends = [&](std::string_view suf) {
        return rel.size() >= suf.size() && rel.substr(rel.size() - suf.size()) == suf;
    };
    if (ends(".pyc") || ends(".pyo")) return true;
    for (std::string_view dir : {"__pycache__", ".pytest_cache"}) {
        std::string mid = "/" + std::string(dir) + "/";
        if (rel.substr(0, dir.size() + 1) == std::string(dir) + "/" || rel.find(mid) != std::string_view::npos)
            return true;
    }
    return false;
}

namespace {

bool is_protected_change(const std::vector<std::string>& protect, const FileChange& c) {
    return path_protected(protect, c.path) || (!c.old_path.empty() && path_protected(protect, c.old_path));
}

struct ChangeProblem {
    enum Kind { None, Revert, Hold } kind = None;
    std::string why;
};

// What the safeguards say about one changed file.
ChangeProblem change_problem(const Project& project, const std::vector<std::string>& protect, const Snapshot& before,
                             const Snapshot& after, const FileChange& c) {
    if (is_protected_change(protect, c)) return {ChangeProblem::Revert, "a protected path was changed"};
    if (c.status == 'D' && is_test_path(c.path)) return {ChangeProblem::Revert, "a test was deleted"};
    if (c.status == 'R' && is_test_path(c.old_path) && !is_test_path(c.path))
        return {ChangeProblem::Revert, "a test was moved out of the tests (" + c.old_path + ")"};
    if (c.status == 'D' || c.binary) return {};
    auto lines = added_lines(project, before, after, c.path);
    for (const auto& l : lines)
        for (const auto& [re, what] : secret_patterns())
            if (std::regex_search(l, re)) return {ChangeProblem::Revert, std::string("it added ") + what};
    if (is_test_path(c.path))
        for (const auto& l : lines)
            if (std::regex_search(l, skip_pattern()))
                return {ChangeProblem::Hold, "a test was skipped or disabled (" + first_line(std::string(trim(l)), 80) + ")"};
    return {};
}

}  // namespace

bool restore_path(const Project& project, const Snapshot& snap, const std::string& path) {
    GitOut exists = git(project, snap.git_dir, {"cat-file", "-e", snap.tree + ":" + path});
    if (exists.code == 0) {
        GitOut r = git(project, snap.git_dir, {"restore", "--source=" + snap.tree, "--worktree", "--", path});
        return r.code == 0;
    }
    std::error_code ec;
    fs::remove(project.dir() / path, ec);
    return !ec;
}

ChangeReview review_changes(const Project& project, const GuardPolicy& policy, const Snapshot& before,
                            const Snapshot& after, bool revert, bool sandboxed) {
    ChangeReview r;
    r.files = diff_snapshots(project, before, after);
    std::vector<std::string> protect = protected_globs(project, policy);
    if (sandboxed) {
        // Only what the sandbox does not keep read-only (the project's extra paths).
        std::vector<std::string> extra = policy.protect;
        for (const auto& g : protect)
            if (!g.empty() && g[0] == '!') extra.push_back(g);
        protect = extra;
    }
    auto undo = [&](const FileChange& c, const std::string& why) {
        std::string what = c.path + ": " + why;
        if (revert) {
            bool ok = restore_path(project, before, c.path);
            if (!c.old_path.empty()) ok = restore_path(project, before, c.old_path) && ok;
            what += ok ? " (reverted)" : " (could not revert)";
        }
        r.reverted.push_back(what);
        r.hold = true;
    };
    int binaries = 0;
    for (const FileChange& c : r.files) {
        r.lines += c.added + c.removed;
        ChangeProblem pr = change_problem(project, protect, before, after, c);
        if (pr.kind == ChangeProblem::Revert) {
            undo(c, pr.why);
            continue;
        }
        if (pr.kind == ChangeProblem::Hold) {
            r.flags.push_back(c.path + ": " + pr.why);
            r.hold = true;
            continue;
        }
        if (c.binary && c.status == 'A') ++binaries;
    }
    if (static_cast<int>(r.files.size()) > policy.max_files)
        r.flags.push_back(std::to_string(r.files.size()) + " files changed (more than " + std::to_string(policy.max_files) + ")");
    if (r.lines > policy.max_lines)
        r.flags.push_back(std::to_string(r.lines) + " lines changed (more than " + std::to_string(policy.max_lines) + ")");
    if (binaries) r.flags.push_back(std::to_string(binaries) + " binary file(s) added");
    return r;
}

namespace {

std::string normal_rel(std::string p) {
    while (starts_with(p, "./")) p.erase(0, 2);
    while (p.size() > 1 && p.back() == '/') p.pop_back();
    return p;
}

bool path_covers(const std::string& want, const std::string& path) {
    return want == "." || want == path || starts_with(path, want + "/");
}

}  // namespace

namespace {

struct HookRun {
    bool ran = false;
    int code = 0;
    std::string output;  ///< the last lines, stdout and stderr
};

// A git hook of the project (core.hooksPath honored), in the sandbox (a
// hooks path inside the project is the agents' to write), on the checkout
// `tree_dir` of what is being committed.
HookRun run_hook(const Project& p, const std::string& name, const std::vector<std::string>& args,
                 std::map<std::string, std::string> env, const fs::path& tree_dir, const fs::path& run_dir) {
    HookRun h;
    GitOut gp = git(p, "", {"rev-parse", "--git-path", "hooks/" + name});
    fs::path hook = one(gp);
    if (hook.empty()) return h;
    if (hook.is_relative()) hook = p.dir() / hook;
    if (!fs::is_regular_file(hook) || ::access(hook.c_str(), X_OK) != 0) return h;
    h.ran = true;
    ProcessOptions o;
    try {
        o.argv = sandbox_argv(p, "hook", run_dir);
    } catch (const std::exception& e) {
        h.code = -1;
        h.output = e.what();
        return h;
    }
    o.argv.insert(o.argv.end(), {"/bin/sh", "-c", "cd \"$0\" && hook=\"$1\" && shift && exec \"$hook\" \"$@\" 2>&1",
                                 tree_dir.string(), hook.string()});
    o.argv.insert(o.argv.end(), args.begin(), args.end());
    o.cwd = p.dir();
    env["GIT_EDITOR"] = ":";
    env["GIT_TERMINAL_PROMPT"] = "0";
    o.env = std::move(env);
    o.timeout_seconds = p.config().commit_hook_timeout;
    std::vector<std::string> lines;
    ProcessResult r = run_process(o, [&](std::string_view l) { lines.emplace_back(l); });
    h.code = r.timed_out ? -2 : r.exit_code;
    std::size_t from = lines.size() > 30 ? lines.size() - 30 : 0;
    for (std::size_t i = from; i < lines.size(); ++i) h.output += lines[i] + "\n";
    if (r.timed_out) h.output += "(timed out after " + std::to_string(o.timeout_seconds) + " s)\n";
    if (!r.error.empty()) h.output += r.error + "\n";
    return h;
}

std::string hook_failure(const std::string& name, const HookRun& h) {
    std::string out = "the " + name + " hook failed (exit " + std::to_string(h.code) + "), nothing was committed";
    if (!trim(h.output).empty()) out += ". Its output:\n" + h.output;
    return out;
}

}  // namespace

RunCommit commit_run_work(const Project& project, const GuardPolicy& policy, const Snapshot& before,
                          const CommitRequest& req, const CommitIdentity& who, const fs::path& work_dir) {
    RunCommit out;
    auto fail = [&](std::string why) {
        out.error = std::move(why);
        return out;
    };
    if (!before.git_dir.empty() || !is_git_repo(project))
        return fail("the project is not a git repository, so there is nothing to commit to (myteam keeps "
                    "checkpoints of your work)");
    std::string message(trim(req.message));
    if (message.empty()) return fail("the commit message is empty");
    GitOut branch = git(project, "", {"symbolic-ref", "-q", "HEAD"});
    if (branch.code != 0 || one(branch).empty())
        return fail("HEAD is detached: commits go on a branch, and the operator chooses it");
    for (const char* f : {"MERGE_HEAD", "CHERRY_PICK_HEAD", "REVERT_HEAD", "rebase-merge", "rebase-apply"}) {
        GitOut gp = git(project, "", {"rev-parse", "--git-path", f});
        fs::path gpath = one(gp);
        if (gpath.is_relative()) gpath = project.dir() / gpath;
        if (!one(gp).empty() && fs::exists(gpath))
            return fail(std::string("a git operation is in progress (") + f + "); the operator finishes it first");
    }
    out.branch = one(branch);
    if (starts_with(out.branch, "refs/heads/")) out.branch.erase(0, 11);

    std::string err;
    auto now = take_snapshot(project, "myteam: for a commit by @" + who.handle, &err);
    if (!now) return fail("could not read the work tree: " + err);
    GitOut head = git(project, "", {"rev-parse", "-q", "--verify", "HEAD"});
    const std::string old_head = head.code == 0 ? one(head) : "";
    // What differs from HEAD now (so what an earlier commit took is not taken again).
    std::set<std::string> uncommitted;
    {
        GitOut d = old_head.empty() ? git(project, "", {"ls-tree", "-r", "--name-only", now->tree})
                                    : git(project, "", {"diff", "--name-only", "--no-renames", old_head, now->tree});
        for (const auto& l : d.lines) uncommitted.insert(l);
    }
    // The run's own changes, still uncommitted.
    std::vector<FileChange> candidates;
    for (FileChange& c : diff_snapshots(project, before, *now))
        if (uncommitted.count(c.path) || (!c.old_path.empty() && uncommitted.count(c.old_path))) candidates.push_back(c);

    const auto protect = protected_globs(project, policy);
    std::vector<std::string> wanted;
    for (const auto& p : req.paths)
        if (!trim(p).empty()) wanted.push_back(normal_rel(std::string(trim(p))));
    std::vector<FileChange> chosen;
    std::vector<std::string> unmatched;
    for (const auto& w : wanted) {
        bool any = false;
        for (const auto& c : candidates)
            if (path_covers(w, c.path) || (!c.old_path.empty() && path_covers(w, c.old_path))) any = true;
        if (!any) unmatched.push_back(w);
    }
    if (!unmatched.empty())
        return fail("not changed by your run, or already committed: " + join(unmatched, ", ") +
                    ". You commit your own work; git status shows what is uncommitted.");
    std::vector<std::string> refused;
    for (const auto& c : candidates) {
        bool asked = wanted.empty();
        for (const auto& w : wanted)
            if (path_covers(w, c.path) || (!c.old_path.empty() && path_covers(w, c.old_path))) asked = true;
        if (!asked) continue;
        // Bytecode is never the work, whatever the unit's .gitignore says
        // (commits would sweep __pycache__/*.pyc in); deleting what an
        // earlier commit took is the cleanup, so that one is committed.
        if (c.status != 'D' && is_bytecode(c.path)) {
            out.left_out.push_back(c.path + " (bytecode: never committed; add __pycache__/ and *.pyc to .gitignore)");
            continue;
        }
        if (is_protected_change(protect, c)) {
            if (wanted.empty() || std::find(wanted.begin(), wanted.end(), c.path) == wanted.end()) {
                out.left_out.push_back(c.path + " (protected: myteam or the operator commits it)");
                continue;
            }
            refused.push_back(c.path + ": it is protected; myteam or the operator commits it");
            continue;
        }
        ChangeProblem pr = change_problem(project, protect, before, *now, c);
        if (pr.kind != ChangeProblem::None) {
            refused.push_back(c.path + ": " + pr.why);
            continue;
        }
        chosen.push_back(c);
    }
    if (!refused.empty())
        return fail("refused, nothing was committed: " + join(refused, "; ") +
                    ". Fix these, or leave the files out with paths, and commit again.");
    if (chosen.empty()) return fail("nothing to commit: your run has no uncommitted changes" +
                                    std::string(out.left_out.empty() ? "" : " outside the protected paths"));

    // The commit: HEAD's tree with the chosen files as they are now, in an
    // index of its own (the user's index is synced afterwards).
    const fs::path wd = (work_dir.empty() ? project.state_dir() / "commit" : work_dir) / ("c" + file_timestamp());
    std::error_code ec;
    fs::remove_all(wd, ec);
    fs::create_directories(wd);
    struct Cleanup {
        fs::path dir;
        ~Cleanup() {
            std::error_code e;
            fs::remove_all(dir, e);
        }
    } cleanup{wd};
    const fs::path index = fs::absolute(wd / "index");
    std::map<std::string, std::string> env{{"GIT_INDEX_FILE", index.string()}};
    GitOut rt = old_head.empty() ? git(project, "", {"read-tree", "--empty"}, env)
                                 : git(project, "", {"read-tree", old_head}, env);
    if (rt.code != 0) return fail("git read-tree failed");
    std::vector<std::string> paths;
    for (const auto& c : chosen) {
        paths.push_back(c.path);
        if (!c.old_path.empty()) paths.push_back(c.old_path);
    }
    for (const auto& p : paths) {
        GitOut ls = git(project, "", {"ls-tree", now->tree, "--", p});
        std::string entry = one(ls);  // "<mode> blob <sha>\t<path>"
        GitOut u;
        if (!entry.empty()) {
            std::size_t sp1 = entry.find(' '), sp2 = entry.find(' ', sp1 + 1), tab = entry.find('\t');
            std::string mode = entry.substr(0, sp1), sha = entry.substr(sp2 + 1, tab - sp2 - 1);
            u = git(project, "", {"update-index", "--add", "--cacheinfo", mode + "," + sha + "," + p}, env);
        } else {
            u = git(project, "", {"update-index", "--force-remove", "--", p}, env);
        }
        if (u.code != 0) return fail("git update-index failed for " + p);
        out.files.push_back(p);
    }
    GitOut staged = git(project, "", {"write-tree"}, env);
    if (staged.code != 0 || one(staged).empty()) return fail("git write-tree failed");
    std::string tree = one(staged);

    std::string full = message + "\n";
    std::string trailers;
    if (!who.task.empty()) trailers += "Myteam-Task: " + who.task + "\n";
    if (!who.run.empty()) trailers += "Myteam-Run: " + who.run + "\n";
    if (!trailers.empty()) full += "\n" + trailers;
    std::map<std::string, std::string> id{{"GIT_AUTHOR_NAME", who.name.empty() ? who.handle : who.name},
                                          {"GIT_AUTHOR_EMAIL", who.email.empty() ? who.handle + "@myteam.invalid" : who.email}};
    // The committer: the operator, as git knows them (else myteam).
    GitOut cn = git(project, "", {"config", "user.name"}), ce = git(project, "", {"config", "user.email"});
    if (cn.code == 0 && !one(cn).empty()) id["GIT_COMMITTER_NAME"] = one(cn);
    if (ce.code == 0 && !one(ce).empty()) id["GIT_COMMITTER_EMAIL"] = one(ce);

    // The project's hooks: pre-commit, prepare-commit-msg, commit-msg, on a
    // checkout of exactly what is committed, with an object store of their
    // own (the repository is read-only in the sandbox) over the real one.
    const bool hooks = project.config().commit_hooks;
    std::map<std::string, std::string> henv = id;
    const fs::path tree_dir = fs::absolute(wd / "tree"), objects = fs::absolute(wd / "objects");
    if (hooks) {
        GitOut od = git(project, "", {"rev-parse", "--git-path", "objects"});
        fs::path real_objects = one(od);
        if (real_objects.is_relative()) real_objects = project.dir() / real_objects;
        GitOut gd = git(project, "", {"rev-parse", "--absolute-git-dir"});
        fs::create_directories(objects / "info");
        fs::create_directories(tree_dir);
        henv["GIT_INDEX_FILE"] = index.string();
        henv["GIT_DIR"] = one(gd);
        henv["GIT_WORK_TREE"] = tree_dir.string();
        henv["GIT_OBJECT_DIRECTORY"] = objects.string();
        henv["GIT_ALTERNATE_OBJECT_DIRECTORIES"] = fs::absolute(real_objects).string();
        GitOut co = git(project, "", {"checkout-index", "-a", "--prefix=" + tree_dir.string() + "/"}, env);
        if (co.code != 0) return fail("could not check out the commit for its hooks");
        // The project's tools, which git does not track.
        for (const char* d : {"node_modules", ".venv", "venv", "vendor"})
            if (fs::exists(project.dir() / d) && !fs::exists(tree_dir / d))
                fs::create_directory_symlink(project.dir() / d, tree_dir / d, ec);
    }
    auto hook = [&](const std::string& name, const std::vector<std::string>& args) {
        return hooks ? run_hook(project, name, args, henv, tree_dir, wd) : HookRun{};
    };
    HookRun pre = hook("pre-commit", {});
    if (pre.ran && pre.code != 0) return fail(hook_failure("pre-commit", pre));
    if (pre.ran) out.hooks.push_back("pre-commit");
    const fs::path msg_file = wd / "COMMIT_EDITMSG";
    write_file_atomic(msg_file, full);
    for (const auto& [name, args] : std::vector<std::pair<std::string, std::vector<std::string>>>{
             {"prepare-commit-msg", {msg_file.string(), "message"}}, {"commit-msg", {msg_file.string()}}}) {
        HookRun h = hook(name, args);
        if (h.ran && h.code != 0) return fail(hook_failure(name, h));
        if (h.ran) out.hooks.push_back(name);
    }
    if (hooks) {
        full = read_file(msg_file);
        if (trim(full).empty()) return fail("the commit-msg hook left the message empty");
        // What the hooks staged (a formatter's fixes): only the commit's files.
        std::map<std::string, std::string> objenv = env;
        objenv["GIT_OBJECT_DIRECTORY"] = objects.string();
        objenv["GIT_ALTERNATE_OBJECT_DIRECTORIES"] = henv["GIT_ALTERNATE_OBJECT_DIRECTORIES"];
        GitOut hooked = git(project, "", {"write-tree"}, objenv);
        if (hooked.code != 0 || one(hooked).empty()) return fail("the hooks left an index git cannot read");
        if (one(hooked) != tree) {
            GitOut d = git(project, "", {"diff", "--name-only", "--no-renames", tree, one(hooked)}, objenv);
            std::vector<std::string> outside;
            for (const auto& l : d.lines)
                if (std::find(paths.begin(), paths.end(), l) == paths.end()) outside.push_back(l);
            if (!outside.empty())
                return fail("the hooks staged files outside the commit: " + join(outside, ", ") + "; nothing was committed");
            // Into the repository: the hooks' new objects (content-addressed, so copying is safe).
            for (const auto& e : fs::recursive_directory_iterator(objects, ec)) {
                if (!e.is_regular_file()) continue;
                fs::path rel = fs::relative(e.path(), objects);
                if (rel.begin()->string() == "info") continue;
                fs::path dst = fs::path(henv["GIT_ALTERNATE_OBJECT_DIRECTORIES"]) / rel;
                if (fs::exists(dst)) continue;
                fs::create_directories(dst.parent_path(), ec);
                fs::copy_file(e.path(), dst, ec);
            }
            out.hook_changed = std::vector<std::string>(d.lines.begin(), d.lines.end());
            tree = one(hooked);
        }
    }
    std::vector<std::string> args = {"commit-tree", tree, "-m", full};
    if (!old_head.empty()) args.insert(args.begin() + 2, {"-p", old_head});
    GitOut commit = git(project, "", args, id);
    if (commit.code != 0 || one(commit).empty()) return fail("git commit-tree failed");
    const std::string sha = one(commit);
    GitOut up = git(project, "", {"update-ref", "-m", "myteam: commit by @" + who.handle, "HEAD", sha, old_head});
    if (up.code != 0) return fail("the branch moved while committing; try again");
    // The user's index: the committed paths as HEAD has them now.
    std::vector<std::string> reset = {"reset", "-q", "--"};
    reset.insert(reset.end(), paths.begin(), paths.end());
    git(project, "", reset);
    // What a hook fixed goes into the work tree too, where the agent's file is
    // still as it was staged.
    for (const auto& p : out.hook_changed) {
        GitOut was = git(project, "", {"rev-parse", "-q", "--verify", now->tree + ":" + p});
        GitOut is = git(project, "", {"hash-object", "--", p});
        if (one(was) == one(is)) git(project, "", {"restore", "--source=" + sha, "--worktree", "--", p});
    }
    out.ok = true;
    out.commit = sha;
    out.summary = first_line(trim(full), 100);
    out.message = full;
    if (hooks) {
        HookRun post = hook("post-commit", {});
        if (post.ran) out.hooks.push_back("post-commit");
    }
    return out;
}

UndoResult undo_run(const Project& project, const std::string& key) {
    UndoResult u;
    fs::path run;
    std::error_code ec;
    if (!key.empty() && key[0] == '^') {
        // The latest run of the task that has a checkpoint.
        std::vector<fs::path> runs;
        for (const auto& e : fs::directory_iterator(project.runs_dir(), ec))
            if (fs::exists(e.path() / "guard.json") && fs::exists(e.path() / "run.json")) runs.push_back(e.path());
        std::sort(runs.rbegin(), runs.rend());
        for (const auto& r : runs) {
            auto j = Json::parse(read_file(r / "run.json"));
            if (const Json* t = j.find("task"); t && t->is_string() && t->as_string() == key) {
                run = r;
                break;
            }
        }
    } else {
        run = project.runs_dir() / key;
    }
    if (run.empty() || !fs::exists(run / "guard.json")) {
        u.problems.push_back("no run with a checkpoint for " + key);
        return u;
    }
    u.run = run.filename().string();
    Json g = Json::parse(read_file(run / "guard.json"));
    auto s = [&](const Json* j, const char* k) {
        const Json* v = j ? j->find(k) : nullptr;
        return v && v->is_string() ? v->as_string() : std::string();
    };
    const std::string gd = s(&g, "git_dir");
    Snapshot before{s(g.find("before"), "commit"), s(g.find("before"), "tree"), gd};
    Snapshot after{s(g.find("after"), "commit"), s(g.find("after"), "tree"), gd};
    if (before.tree.empty() || after.tree.empty()) {
        u.problems.push_back("the run's checkpoints are incomplete");
        return u;
    }
    if (auto j = try_read_file(run / "run.json")) {
        Json rj = Json::parse(*j);
        u.task = s(&rj, "task");
    }
    if (const Json* cs = g.find("commits"); cs && cs->is_array())
        for (const Json& c : cs->as_array())
            if (c.is_string()) u.commits.push_back(c.as_string());
    for (const FileChange& c : diff_snapshots(project, before, after)) {
        // Only what nobody changed since the run.
        GitOut now = git(project, gd, {"hash-object", "--", c.path});
        GitOut then = git(project, gd, {"rev-parse", "-q", "--verify", after.tree + ":" + c.path});
        const bool gone_now = !fs::exists(project.dir() / c.path);
        const bool same = (c.status == 'D') ? gone_now : (!gone_now && one(now) == one(then));
        if (!same) {
            u.skipped.push_back(c.path);
            continue;
        }
        bool ok = restore_path(project, before, c.path);
        if (!c.old_path.empty()) ok = restore_path(project, before, c.old_path) && ok;
        if (ok) u.restored.push_back(c.path);
        else u.problems.push_back("could not restore " + c.path);
    }
    // The task goes back to [ ].
    if (!u.task.empty() && u.task[0] == '^') {
        Plan plan = Plan::load(project.plan_path());
        if (const TaskRef* t = plan.find(u.task)) {
            PlanEditor ed(plan);
            ed.set_status(*t, taskml::TaskStatus::Pending);
            ed.add_note(*t, "Undone " + iso_timestamp().substr(0, 10) + " by the operator: run " + u.run);
            CommitResult c = ed.commit();
            if (!c.ok) u.problems.push_back("plan not updated: " + c.error);
        }
    }
    return u;
}

}  // namespace myteam
