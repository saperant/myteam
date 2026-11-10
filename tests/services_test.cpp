#include <gtest/gtest.h>

#include <algorithm>
#include <sys/stat.h>

#include "myteam/commits.hpp"
#include "myteam/guard.hpp"
#include "myteam/loop.hpp"
#include "myteam/mcp.hpp"
#include "myteam/services.hpp"
#include "myteam/util.hpp"
#include "test_util.hpp"

using namespace myteam;
using myteam::test::TempDir;

namespace {

bool has(const std::string& s, const std::string& what) { return s.find(what) != std::string::npos; }

// A stand-in podman: containers are files (state|spec) in state/, every call is logged.
const char* kFakePodman = R"(#!/bin/sh
d="$(dirname "$0")/state"; mkdir -p "$d"; echo "$*" >> "$d/../calls.log"
case "$1" in
  container) n="$5"; if [ -f "$d/$n" ]; then cat "$d/$n"; else echo "Error: no such container $n"; exit 125; fi ;;
  run) shift; name=""; spec=""
       while [ $# -gt 0 ]; do case "$1" in
         --name) name="$2"; shift ;;
         --label) case "$2" in myteam.spec=*) spec="${2#myteam.spec=}" ;; esac; shift ;;
       esac; shift; done
       echo "running|$spec" > "$d/$name"; echo 0123abcd ;;
  rm) n="$3"; rm -f "$d/$n" ;;
  start) n="$2"; s=$(cut -d'|' -f2 "$d/$n"); echo "running|$s" > "$d/$n" ;;
  restart) [ -f "$d/$2" ] || { echo "Error: no container $2"; exit 125; } ;;
  logs) echo "database system is ready to accept connections" ;;
  volume) echo "$4" ;;
esac
)";

struct World {
    TempDir dir;
    std::optional<Project> project;
    std::optional<Team> team;
    std::string podman;
    explicit World(const std::string& services) {
        dir.write("fake/podman", kFakePodman);
        ::chmod((dir / "fake/podman").c_str(), 0755);
        podman = (dir / "fake/podman").string();
        dir.write("team/team.md", "---\nid: t\nname: T\nproduct_owner: po\nmodels: {default: claude:sonnet}\n---\n");
        dir.write("team/members/po.md", "---\nname: Po\nrole: product-owner\n---\nPrioritize.\n");
        dir.write("team/members/dev.md", "---\nname: Dev\nrole: developer\n---\nCode.\n");
        dir.write("proj/PLAN.md", "# P\n\n- [ ] Use the database @dev ^db\n");
        dir.write("proj/deploy/nginx.conf", "server { listen 8080; }\n");
        dir.write("proj/.myteam/config.yaml", "services_podman: [\"" + podman + "\"]\nservices:\n" + services);
        project = Project::open(dir / "proj");
        team = load_team(dir / "team");
    }
    std::string calls() const { return dir.read("fake/calls.log"); }
    void set_state(const std::string& container, const std::string& state) {
        std::string s = dir.read("fake/state/" + container);
        dir.write("fake/state/" + container, state + s.substr(s.find('|')));
    }
};

const char* kDbAndProxy =
    "  db:\n"
    "    image: docker.io/library/postgres:16-alpine\n"
    "    ports: [\"5432:5432\"]\n"
    "    env: {POSTGRES_PASSWORD: secret}\n"
    "    data: /var/lib/postgresql/data\n"
    "    agent_env: {DATABASE_URL: \"postgres://postgres:secret@127.0.0.1:5432/postgres\"}\n"
    "    description: The project's PostgreSQL\n"
    "  proxy:\n"
    "    image: docker.io/library/nginx:alpine\n"
    "    network: host\n"
    "    files: {/etc/nginx/conf.d/default.conf: deploy/nginx.conf}\n";

}  // namespace

TEST(Services, DefinitionsAreCheckedStrictly) {
    World w("  ok:\n    image: x\n    ports: [\"127.0.0.1:1:2\"]\n"
            "  open:\n    image: x\n    ports: [\"0.0.0.0:5432:5432\"]\n"
            "  out:\n    image: x\n    files: {/etc/a: ../../etc/passwd}\n"
            "  state:\n    image: x\n    files: {/etc/a: .myteam/config.yaml}\n"
            "  Bad_Name:\n    image: x\n");
    const auto& s = w.project->config().services;
    ASSERT_EQ(s.size(), 5u);
    EXPECT_TRUE(service_problems(*w.project, s[0]).empty());
    EXPECT_TRUE(has(join(service_problems(*w.project, s[1]), ";"), "only on 127.0.0.1"));
    EXPECT_TRUE(has(join(service_problems(*w.project, s[2]), ";"), "only from inside the project"));
    EXPECT_TRUE(has(join(service_problems(*w.project, s[3]), ";"), "not .myteam/"));
    EXPECT_FALSE(service_problems(*w.project, s[4]).empty());
    // An invalid one is not started; the valid one is.
    ServicesResult r = ensure_services(*w.project);
    EXPECT_EQ(r.problems.size(), 4u);
    const std::string calls = w.calls();
    EXPECT_TRUE(has(calls, "run -d --name myteam-proj-ok"));  // only the valid one ran
    EXPECT_FALSE(has(calls, "0.0.0.0"));
    EXPECT_FALSE(has(calls, "passwd"));
    EXPECT_FALSE(has(calls, "myteam-proj-open"));
}

TEST(Services, RunAsDefinedRecreatedWhenChangedStartedWhenStopped) {
    World w(kDbAndProxy);
    ServicesResult r = ensure_services(*w.project);
    EXPECT_TRUE(r.problems.empty()) << r.problems[0];
    EXPECT_EQ(r.notes.size(), 2u);
    const std::string db = service_container(*w.project, w.project->config().services[0]);
    EXPECT_TRUE(has(db, "myteam-proj-db"));
    std::string c = w.calls();
    EXPECT_TRUE(has(c, "run -d --name " + db + " --label myteam.project=")) << c;
    EXPECT_TRUE(has(c, "-p 127.0.0.1:5432:5432")) << c;
    EXPECT_TRUE(has(c, "-e POSTGRES_PASSWORD=secret")) << c;
    EXPECT_TRUE(has(c, "-v " + db + "-data:/var/lib/postgresql/data")) << c;
    EXPECT_TRUE(has(c, "--network host")) << c;
    EXPECT_TRUE(has(c, "/deploy/nginx.conf:/etc/nginx/conf.d/default.conf:ro,z")) << c;

    // Running as defined: nothing to do.
    EXPECT_TRUE(ensure_services(*w.project).notes.empty());
    // Stopped: started again.
    w.set_state(db, "exited");
    r = ensure_services(*w.project);
    EXPECT_EQ(r.notes, std::vector<std::string>{"service db started again"});
    // Changed: recreated.
    std::string cfg = w.dir.read("proj/.myteam/config.yaml");
    cfg.replace(cfg.find("secret}"), 6, "better");
    w.dir.write("proj/.myteam/config.yaml", cfg);
    w.project = Project::open(w.dir / "proj");
    EXPECT_EQ(service_status(*w.project)[0].error, "its definition changed: recreated on the next run");
    r = ensure_services(*w.project);
    EXPECT_TRUE(std::any_of(r.notes.begin(), r.notes.end(), [](const std::string& n) { return has(n, "recreated"); }));
    EXPECT_TRUE(has(w.calls(), "rm -f " + db));

    // The operations.
    EXPECT_TRUE(has(service_action(*w.project, "logs", "db").text, "ready to accept connections"));
    EXPECT_TRUE(service_action(*w.project, "restart", "db").ok);
    ServiceAnswer reset = service_action(*w.project, "reset", "db");
    EXPECT_TRUE(reset.ok) << reset.text;
    EXPECT_TRUE(has(w.calls(), "volume rm -f " + db + "-data"));
    EXPECT_FALSE(service_action(*w.project, "logs", "nope").ok);
    EXPECT_TRUE(has(service_action(*w.project, "status", "").text, "- db (docker.io/library/postgres:16-alpine): running"));

    // Down, and with --purge their data too.
    stop_services(*w.project, true);
    EXPECT_EQ(service_status(*w.project)[0].state, "missing");
}

TEST(Services, AgentsGetTheAddressesTheBriefAndTheTools) {
    World w(kDbAndProxy);
    std::string brief = services_brief(*w.project, true);
    EXPECT_TRUE(has(brief, "- db: docker.io/library/postgres:16-alpine. The project's PostgreSQL"));
    EXPECT_TRUE(has(brief, "on 127.0.0.1:5432:5432"));
    EXPECT_TRUE(has(brief, "DATABASE_URL=postgres://postgres:secret@127.0.0.1:5432/postgres"));
    EXPECT_TRUE(has(brief, "service_restart"));
    EXPECT_EQ(service_agent_env(*w.project).at("DATABASE_URL"), "postgres://postgres:secret@127.0.0.1:5432/postgres");

    // Through the broker, as from the sandbox.
    const fs::path run = w.dir / "proj/.myteam/runs/r";
    ensure_services(*w.project);
    CommitBroker broker(*w.project, std::nullopt, {"dev", "Dev", "", "^db", "r"}, run);
    broker.start();
    McpServer mcp({*w.project, *w.team, "dev", "db", run / kMcpReportFile, false, true});
    EXPECT_TRUE(has(mcp.tools().dump(), "service_reset"));
    EXPECT_FALSE(has(mcp.tools().dump(), "git_commit"));
    taskml::Json a = taskml::Json::object();
    a.set("name", "db");
    taskml::Json r = mcp.call("service_logs", a);
    EXPECT_TRUE(has(r.dump(), "ready to accept connections")) << r.dump();
    taskml::Json bad = mcp.call("git_commit", a);  // not offered: no such tool here
    broker.stop();

    // The guard points containers to the services.
    static GuardPolicy policy;
    GuardContext g = guard_context(*w.project, policy, w.project->dir());
    GuardDecision d = check_command(g, "podman run -v /:/host alpine");
    EXPECT_FALSE(d.allow);
    EXPECT_TRUE(has(d.reason, "service_* tools"));
}

namespace {
class Recorder : public AgentRunner {
public:
    explicit Recorder(std::vector<RunRequest>* log) : AgentRunner({}), log_(log) {}
    std::string id() const override { return "claude"; }
    std::string start_problem() const override { return ""; }
    bool supports_mcp() const override { return true; }
    std::vector<std::string> command(const RunRequest&) const override { return {"x"}; }
    void parse_line(std::string_view, StreamState&, const EventSink&) const override {}
    RunResult run(const RunRequest& req, const EventSink&) override {
        log_->push_back(req);
        fs::create_directories(req.run_dir);
        RunResult r;
        r.ok = true;
        r.final_text = "<task_status>done</task_status><work_summary>Used it.</work_summary>";
        return r;
    }

private:
    std::vector<RunRequest>* log_;
};
}  // namespace

TEST(Services, ARoundStartsThemAndTheAgentKnowsThem) {
    World w(kDbAndProxy);
    std::vector<RunRequest> runs;
    Loop loop(*w.project, *w.team, [&](const std::string&) { return std::make_unique<Recorder>(&runs); });
    LoopOptions o;
    o.mcp_command = {"/usr/bin/myteam", "mcp"};
    std::vector<std::string> notes;
    o.on_note = [&](const std::string& n) { notes.push_back(n); };
    loop.iterate(o);
    EXPECT_TRUE(std::any_of(notes.begin(), notes.end(), [](const std::string& n) { return has(n, "service db started"); }));
    ASSERT_EQ(runs.size(), 1u);
    EXPECT_EQ(runs[0].env.at("DATABASE_URL"), "postgres://postgres:secret@127.0.0.1:5432/postgres");
    EXPECT_TRUE(has(runs[0].prompt, "# Services\n"));
    EXPECT_TRUE(has(runs[0].prompt, "service_reset"));
    ASSERT_TRUE(runs[0].mcp);
    const auto& cmd = runs[0].mcp->command;
    EXPECT_NE(std::find(cmd.begin(), cmd.end(), "--services"), cmd.end());
}

TEST(Services, TheContextSaysWhatIsProtectedAndWhatAServiceWaitsFor) {
    World w(kDbAndProxy);
    fs::remove(w.dir / "proj/deploy/nginx.conf");
    std::string cfg = w.dir.read("proj/.myteam/config.yaml");
    w.dir.write("proj/.myteam/config.yaml", cfg + "guard:\n  unprotect: [\".github/workflows/**\"]\n"
                                            "notes: [\"A headless Chromium is in ~/.cache/ms-playwright\"]\n");
    w.project = Project::open(w.dir / "proj");
    EXPECT_TRUE(has(services_brief(*w.project, true), "it starts once deploy/nginx.conf exists"));
    std::vector<RunRequest> runs;
    Loop loop(*w.project, *w.team, [&](const std::string&) { return std::make_unique<Recorder>(&runs); });
    LoopOptions o;
    o.dry_run = true;
    auto plans = loop.preview(o);
    ASSERT_EQ(plans.size(), 1u);
    std::string ctx = plans[0].context.render();
    EXPECT_TRUE(has(ctx, "# Protected paths")) << ctx;
    EXPECT_TRUE(has(ctx, "The safeguards refuse writes to: PLAN.md, CHANGELOG.md, .myteam/**"));
    EXPECT_TRUE(has(ctx, "Yours to write, although they look protected: .myteam/project.md, .github/workflows/**"));
    EXPECT_FALSE(has(ctx, "Besides the project, writes go to")) << ctx;  // only when there are such directories
    // A unit with writes: outside it is told where they are before a write is refused.
    w.dir.write("shared/README.md", "# shared\n");
    w.dir.write("proj/.myteam/config.yaml", w.dir.read("proj/.myteam/config.yaml") + "writable: [\"../shared\"]\n");
    w.project = Project::open(w.dir / "proj");
    ASSERT_EQ(w.project->write_paths().size(), 1u) << w.dir.read("proj/.myteam/config.yaml");
    Loop loop2(*w.project, *w.team, [&](const std::string&) { return std::make_unique<Recorder>(&runs); });
    auto plans2 = loop2.preview(o);
    ASSERT_EQ(plans2.size(), 1u);
    std::string ctx2 = plans2[0].context.render();
    EXPECT_TRUE(has(ctx2, "Besides the project, writes go to: ")) << ctx2;
    EXPECT_TRUE(has(ctx2, "shared (and the temp directory).")) << ctx2;
    EXPECT_TRUE(has(ctx, "# From the operator\n\n- A headless Chromium is in ~/.cache/ms-playwright\n")) << ctx;
}

TEST(Services, ANotRunningServiceSaysWhy) {
    World w(kDbAndProxy);
    fs::remove(w.dir / "proj/deploy/nginx.conf");
    ServiceAnswer logs = service_action(*w.project, "logs", "proxy");
    EXPECT_FALSE(logs.ok);
    EXPECT_TRUE(has(logs.text, "proxy is not running (waiting): for deploy/nginx.conf (it does not exist yet)")) << logs.text;
    ServiceAnswer restart = service_action(*w.project, "restart", "proxy");
    EXPECT_FALSE(restart.ok);
    EXPECT_TRUE(has(restart.text, "deploy/nginx.conf does not exist yet")) << restart.text;
}
