#include "myteam/runner.hpp"

#include <cstdlib>
#include <fstream>
#include <unistd.h>

#include <taskml/json.hpp>

#include "myteam/process.hpp"
#include "myteam/team.hpp"
#include "myteam/util.hpp"
#include "runners/runners.hpp"

namespace myteam {

std::optional<Permission> permission_from_string(std::string_view s) {
    std::string v = lower(trim(s));
    if (v == "read-only" || v == "readonly" || v == "read") return Permission::ReadOnly;
    if (v == "edit" || v == "edit-files" || v == "write") return Permission::EditFiles;
    if (v == "full" || v == "yolo") return Permission::Full;
    return std::nullopt;
}

const char* to_string(Permission p) {
    switch (p) {
        case Permission::ReadOnly: return "read-only";
        case Permission::EditFiles: return "edit";
        case Permission::Full: return "full";
    }
    return "edit";
}

fs::path AgentRunner::executable() const {
    std::string cmd = settings_.command.empty() ? id() : settings_.command;
    if (auto p = find_executable(cmd)) return *p;
    return cmd;
}

bool AgentRunner::available() const {
    return find_executable(settings_.command.empty() ? id() : settings_.command).has_value();
}

std::string AgentRunner::start_problem() const {
    const std::string cmd = settings_.command.empty() ? id() : settings_.command;
    auto exe = find_executable(cmd);
    if (!exe) return cmd + " is not installed";
    std::error_code ec;
    if (!fs::exists(*exe, ec)) return exe->string() + " is a broken link";
    // A script: its interpreter must be there too.
    std::string head = try_read_file(*exe).value_or("").substr(0, 256);
    if (!starts_with(head, "#!")) return "";
    std::string line = head.substr(2, head.find('\n') == std::string::npos ? std::string::npos : head.find('\n') - 2);
    std::string interp(trim(line));
    std::string rest;
    if (auto sp = interp.find_first_of(" \t"); sp != std::string::npos) {
        rest = std::string(trim(interp.substr(sp)));
        interp = interp.substr(0, sp);
    }
    if (fs::path(interp).filename() == "env" && !rest.empty()) {
        if (starts_with(rest, "-S")) rest = std::string(trim(rest.substr(2)));
        std::string prog = rest.substr(0, rest.find_first_of(" \t"));
        if (!find_executable(prog)) return exe->string() + " runs " + prog + ", which is not on $PATH";
        return "";
    }
    if (!fs::exists(interp, ec)) {
        std::string why = exe->string() + " cannot start: its interpreter " + interp;
        std::error_code e2;
        fs::path target = fs::is_symlink(interp, e2) ? fs::read_symlink(interp, e2) : fs::path();
        if (!target.empty()) why += " -> " + target.string();
        return why + " is missing (installed for another system? reinstall it here, or set runners." + id() +
               ".command)";
    }
    return "";
}

void AgentRunner::finish(const RunRequest&, StreamState&) const {}

const char* to_string(Failure f) {
    switch (f) {
        case Failure::None: return "none";
        case Failure::NotAvailable: return "not-available";
        case Failure::Auth: return "auth";
        case Failure::RateLimit: return "rate-limit";
        case Failure::Limit: return "limit";
        case Failure::Crash: return "crash";
    }
    return "crash";
}

Failure classify_failure(int exit_code, bool timed_out, std::string_view error, std::string_view stop_reason,
                         std::string_view stderr_tail) {
    const std::string all = lower(std::string(error) + "\n" + std::string(stop_reason) + "\n" + std::string(stderr_tail));
    auto has = [&](std::initializer_list<const char*> words) {
        for (const char* w : words)
            if (all.find(w) != std::string::npos) return true;
        return false;
    };
    if (exit_code == -1 || has({"cannot run ", "unknown runner", "command not found", "no such file or directory"}))
        return Failure::NotAvailable;
    if (has({"not logged in", "please log in", "please run /login", "authentication", "unauthorized",
             "invalid api key", "invalid x-api-key", "api key not", "no api key", "401 "}))
        return Failure::Auth;
    if (has({"rate limit", "rate_limit", "ratelimit", "429", "overloaded", "too many requests", "quota",
             "usage limit", "capacity",
             // Claude's plan limits: "You've hit your 5-hour limit · resets 6:30pm" (not a crash).
             "hit your", "-hour limit", "weekly limit", "limit · resets", "limit reached"}))
        return Failure::RateLimit;
    if (timed_out || has({"max_turns", "max turns", "turn_limit", "error_max_budget", "max_budget", "price_limit",
                          "token_limit", "time_limit", "budget exceeded"}))
        return Failure::Limit;
    if (exit_code == 0 && error.empty()) return Failure::None;
    return Failure::Crash;
}

RunResult AgentRunner::run(const RunRequest& req, const EventSink& sink) {
    fs::create_directories(req.run_dir);
    StreamState state;
    RunResult& res = state.result;
    res.prompt_path = req.run_dir / "prompt.md";
    res.stream_path = req.run_dir / "stream.jsonl";
    res.stderr_path = req.run_dir / "stderr.log";
    write_file_atomic(res.prompt_path, req.prompt);

    ProcessOptions po;
    po.argv = command(req);
    po.argv[0] = executable().string();
    if (!req.sandbox.empty()) po.argv.insert(po.argv.begin(), req.sandbox.begin(), req.sandbox.end());
    po.cwd = req.workdir;
    po.env = settings_.env;
    // The project's virtualenv, as if activated: its python and pip come first.
    for (const char* venv : {".venv", "venv"}) {
        const fs::path dir = req.workdir / venv;
        std::error_code ec;
        // (Its python must run here: a venv made for another system is left alone.)
        if (req.workdir.empty() || !fs::exists(dir / "bin" / "python", ec)) continue;
        const char* path = std::getenv("PATH");
        po.env["VIRTUAL_ENV"] = dir.string();
        po.env["PATH"] = (dir / "bin").string() + (path && *path ? ":" + std::string(path) : "");
        po.env.erase("PYTHONHOME");
        break;
    }
    for (const auto& [k, v] : req.env) po.env[k] = v;
    for (auto& [k, v] : environment(req)) po.env[k] = v;
    po.stdin_text = stdin_text(req);
    po.stderr_path = res.stderr_path;
    po.timeout_seconds = req.timeout_seconds;
    {
        std::string cmdline;
        for (const auto& a : po.argv) cmdline += (cmdline.empty() ? "" : " ") + a;
        write_file_atomic(req.run_dir / "command.txt", cmdline + "\n");
    }

    // What the run is, for whoever watches (myteam json runs): written now and
    // again when it ends.
    taskml::Json meta = taskml::Json::object();
    meta.set("runner", id());
    meta.set("model", req.model);
    meta.set("member", req.member);
    meta.set("what", req.what);
    meta.set("started", iso_timestamp());
    meta.set("pid", static_cast<int>(::getpid()));
    write_file_atomic(req.run_dir / "meta.json", meta.dump(2) + "\n");

    // A CLI that cannot start: say why, without starting it.
    if (std::string why = start_problem(); !why.empty()) {
        res.error = why;
        res.failure = Failure::NotAvailable;
        write_file_atomic(req.run_dir / "result.md", "");
        meta.set("finished", iso_timestamp());
        meta.set("ok", false);
        meta.set("failure", to_string(res.failure));
        write_file_atomic(req.run_dir / "meta.json", meta.dump(2) + "\n");
        if (sink) sink({AgentEvent::Kind::Error, why, ""});
        return std::move(res);
    }

    if (sink) sink({AgentEvent::Kind::Started, id() + (req.model.empty() ? "" : ":" + req.model), ""});
    std::ofstream stream(res.stream_path, std::ios::binary | std::ios::trunc);
    ProcessResult pr = run_process(po, [&](std::string_view line) {
        stream << line << '\n';
        stream.flush();
        if (!trim(line).empty()) parse_line(line, state, sink);
    });
    stream.close();

    res.exit_code = pr.exit_code;
    res.timed_out = pr.timed_out;
    if (!pr.error.empty()) res.error = pr.error;
    finish(req, state);
    if (res.final_text.empty()) res.final_text = state.last_text;
    if (res.timed_out && res.error.empty()) res.error = "timed out after " + std::to_string(req.timeout_seconds) + " s";
    if (res.exit_code != 0 && res.error.empty()) {
        // The last lines of stderr usually say why.
        auto err = try_read_file(res.stderr_path).value_or("");
        auto lines = split_lines(err);
        std::string tail;
        for (std::size_t i = lines.size() > 5 ? lines.size() - 5 : 0; i < lines.size(); ++i) tail += lines[i] + "\n";
        res.error = "exit code " + std::to_string(res.exit_code) + (tail.empty() ? "" : ": " + std::string(trim(tail)));
    }
    res.ok = res.exit_code == 0 && !res.timed_out && res.error.empty();
    if (!res.ok) {
        auto err = try_read_file(res.stderr_path).value_or("");
        if (err.size() > 4000) err = err.substr(err.size() - 4000);
        res.failure = classify_failure(res.exit_code, res.timed_out, res.error, res.stop_reason, err);
        if (res.failure == Failure::None) res.failure = Failure::Crash;
    }
    write_file_atomic(req.run_dir / "result.md", res.final_text);
    meta.set("finished", iso_timestamp());
    meta.set("ok", res.ok);
    meta.set("failure", to_string(res.failure));
    meta.set("cost_usd", res.usage.cost_usd);
    meta.set("turns", res.usage.turns);
    write_file_atomic(req.run_dir / "meta.json", meta.dump(2) + "\n");
    if (sink) sink({AgentEvent::Kind::Finished, res.ok ? "ok" : res.error, ""});
    return std::move(res);
}

ModelRef usable_model(const ModelRef& wanted, const ModelRef& standard,
                      const std::function<std::unique_ptr<AgentRunner>(const std::string&)>& make,
                      std::set<std::string>& told, const std::function<void(const std::string&)>& note) {
    if (wanted == standard || wanted.runner == standard.runner) return wanted;
    auto r = make ? make(wanted.runner) : nullptr;
    const std::string why = r ? r->start_problem() : "";
    if (why.empty()) return wanted;
    if (!told.insert(wanted.runner).second) return standard;
    if (note) note(wanted.runner + " cannot start here (" + first_line(why, 240) + "); members whose model is " +
                   wanted.runner + " use their standard model instead");
    return standard;
}

std::unique_ptr<AgentRunner> make_runner(std::string_view id, RunnerSettings settings) {
    if (id == "claude") return make_claude_runner(std::move(settings));
    if (id == "codex") return make_codex_runner(std::move(settings));
    if (id == "vibe") return make_vibe_runner(std::move(settings));
    return nullptr;
}

std::vector<std::string> runner_ids() { return {"claude", "codex", "vibe"}; }

}  // namespace myteam
