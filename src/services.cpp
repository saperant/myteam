#include "myteam/services.hpp"

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <functional>
#include <optional>
#include <regex>
#include <unistd.h>

#include "myteam/process.hpp"
#include "myteam/util.hpp"

namespace myteam {

namespace fs = std::filesystem;
using taskml::Json;

namespace {

struct Out {
    int code = -1;
    std::vector<std::string> lines;  ///< stdout and stderr, as they came
};

Out podman(const Project& p, std::vector<std::string> args, int timeout = 300) {
    ProcessOptions o;
    // Its stderr with its stdout: what went wrong is in the answer.
    o.argv = {"/bin/sh", "-c", "exec \"$@\" 2>&1", "sh"};
    auto pre = podman_command(p);
    o.argv.insert(o.argv.end(), pre.begin(), pre.end());
    o.argv.insert(o.argv.end(), args.begin(), args.end());
    o.cwd = p.dir();
    o.timeout_seconds = timeout;
    Out out;
    ProcessResult r = run_process(o, [&](std::string_view l) { out.lines.emplace_back(l); });
    out.code = r.exit_code;
    if (!r.error.empty()) out.lines.push_back(r.error);
    return out;
}

std::string tail(const Out& o, std::size_t n = 6) {
    std::string t;
    for (std::size_t i = o.lines.size() > n ? o.lines.size() - n : 0; i < o.lines.size(); ++i) t += o.lines[i] + "\n";
    return std::string(trim(t));
}

const ServiceSpec* find_service(const Project& p, const std::string& name) {
    for (const auto& s : p.config().services)
        if (s.name == name) return &s;
    return nullptr;
}

std::string volume_name(const Project& p, const ServiceSpec& s) { return service_container(p, s) + "-data"; }

// "5432:5432" -> "127.0.0.1:5432:5432"; "" for one that is not on loopback.
std::string loopback_port(const std::string& spec) {
    static const std::regex hc(R"(^(\d{1,5}):(\d{1,5})(/(tcp|udp))?$)");
    static const std::regex ihc(R"(^(127\.0\.0\.1|localhost):(\d{1,5}):(\d{1,5})(/(tcp|udp))?$)");
    static const std::regex c(R"(^(\d{1,5})(/(tcp|udp))?$)");
    std::smatch m;
    if (std::regex_match(spec, m, ihc)) return "127.0.0.1:" + m[2].str() + ":" + m[3].str() + m[4].str();
    if (std::regex_match(spec, m, hc)) return "127.0.0.1:" + m[1].str() + ":" + m[2].str() + m[3].str();
    if (std::regex_match(spec, m, c)) return "127.0.0.1:" + m[1].str() + ":" + m[1].str() + m[2].str();
    return "";
}

std::vector<std::string> run_args(const Project& p, const ServiceSpec& s, const std::string& hash) {
    std::vector<std::string> a = {"run", "-d", "--name", service_container(p, s),
                                  "--label", "myteam.project=" + p.dir().string(),
                                  "--label", "myteam.service=" + s.name,
                                  "--label", "myteam.spec=" + hash,
                                  "--restart", "unless-stopped"};
    if (s.network == "host") a.insert(a.end(), {"--network", "host"});
    for (const auto& port : s.ports) a.insert(a.end(), {"-p", loopback_port(port)});
    for (const auto& [k, v] : s.env) a.insert(a.end(), {"-e", k + "=" + v});
    std::error_code ec;
    const fs::path dir = fs::weakly_canonical(p.dir(), ec);
    for (const auto& [inside, rel] : s.files)
        a.insert(a.end(), {"-v", (dir / rel).string() + ":" + inside + ":ro,z"});
    if (!s.data.empty()) a.insert(a.end(), {"-v", volume_name(p, s) + ":" + s.data});
    a.push_back(s.image);
    a.insert(a.end(), s.command.begin(), s.command.end());
    return a;
}

std::string spec_hash(const Project& p, const ServiceSpec& s) {
    std::string all;
    for (const auto& x : run_args(p, s, "")) all += x + '\x1f';
    char buf[17];
    std::snprintf(buf, sizeof buf, "%016zx", std::hash<std::string>{}(all));
    return buf;
}

// "running|<hash>", or nothing when there is no such container.
std::optional<std::pair<std::string, std::string>> inspect(const Project& p, const std::string& container) {
    Out o = podman(p, {"container", "inspect", "--format", "{{.State.Status}}|{{index .Config.Labels \"myteam.spec\"}}",
                       container},
                   60);
    if (o.code != 0 || o.lines.empty()) return std::nullopt;
    std::string l = o.lines.back();
    auto bar = l.find('|');
    if (bar == std::string::npos) return std::nullopt;
    return std::make_pair(l.substr(0, bar), l.substr(bar + 1));
}

}  // namespace

std::vector<std::string> podman_command(const Project& project) {
    if (!project.config().services_podman.empty()) return project.config().services_podman;
    // The host's podman, through its socket, when there is one: a toolbox's own
    // podman often cannot run containers (no subordinate ids).
    std::string runtime = std::getenv("XDG_RUNTIME_DIR") ? std::getenv("XDG_RUNTIME_DIR")
                                                         : "/run/user/" + std::to_string(::getuid());
    fs::path sock = fs::path(runtime) / "podman" / "podman.sock";
    std::error_code ec;
    if (fs::exists(sock, ec)) return {"podman", "--remote", "--url", "unix://" + sock.string()};
    return {"podman"};
}

std::string service_container(const Project& project, const ServiceSpec& s) {
    return "myteam-" + slug(project.dir().filename().string(), 30) + "-" + s.name;
}

std::vector<std::string> service_problems(const Project& project, const ServiceSpec& s) {
    std::vector<std::string> out;
    static const std::regex name(R"(^[a-z][a-z0-9-]{0,30}$)");
    if (!std::regex_match(s.name, name)) out.push_back("the name must be lower case letters, digits and -");
    if (s.image.empty() || s.image.find_first_of(" \t\n") != std::string::npos) out.push_back("no image");
    if (!s.network.empty() && s.network != "host") out.push_back("network: only host, or none given");
    if (s.network == "host" && !s.ports.empty()) out.push_back("ports and network: host do not go together");
    for (const auto& port : s.ports)
        if (loopback_port(port).empty()) out.push_back("port " + port + ": only on 127.0.0.1 (HOST:CONTAINER)");
    std::error_code ec;
    const fs::path dir = fs::weakly_canonical(project.dir(), ec);
    for (const auto& [inside, rel] : s.files) {
        fs::path f = fs::weakly_canonical(dir / rel, ec);
        auto r = f.lexically_relative(dir).generic_string();
        if (fs::path(rel).is_absolute() || r.empty() || starts_with(r, "..") || starts_with(r, ".myteam") ||
            starts_with(r, ".git"))
            out.push_back("file " + rel + ": only from inside the project (not .myteam/ or .git/)");
        if (inside.empty() || inside[0] != '/') out.push_back("file " + rel + ": its place in the container is a path");
    }
    if (!s.data.empty() && s.data[0] != '/') out.push_back("data: a path in the container");
    return out;
}

ServicesResult ensure_services(const Project& project) {
    ServicesResult r;
    for (const ServiceSpec& s : project.config().services) {
        ServiceStatus st{s.name, service_container(project, s), "", ""};
        auto bad = service_problems(project, s);
        if (!bad.empty()) {
            st.state = "invalid";
            st.error = join(bad, "; ");
            r.problems.push_back("service " + s.name + ": " + st.error);
            r.services.push_back(st);
            continue;
        }
        bool missing = false;
        for (const auto& [inside, rel] : s.files)
            if (!fs::exists(project.dir() / rel)) {
                st.state = "waiting";
                st.error = rel + " does not exist yet";
                missing = true;
            }
        if (missing) {
            r.problems.push_back("service " + s.name + ": " + st.error + " (not started)");
            r.services.push_back(st);
            continue;
        }
        const std::string hash = spec_hash(project, s);
        auto now = inspect(project, st.container);
        if (now && now->second != hash) {
            podman(project, {"rm", "-f", st.container}, 120);
            r.notes.push_back("service " + s.name + ": its definition changed, recreated");
            now.reset();
        }
        if (!now) {
            Out o = podman(project, run_args(project, s, hash), 900);
            if (o.code != 0) {
                st.state = "failed";
                st.error = tail(o);
                r.problems.push_back("service " + s.name + " did not start: " + first_line(st.error, 300));
            } else {
                st.state = "running";
                r.notes.push_back("service " + s.name + " started (" + s.image + ")");
            }
        } else if (now->first != "running") {
            Out o = podman(project, {"start", st.container}, 120);
            st.state = o.code == 0 ? "running" : now->first;
            if (o.code != 0) {
                st.error = tail(o);
                r.problems.push_back("service " + s.name + " did not start: " + first_line(st.error, 300));
            } else {
                r.notes.push_back("service " + s.name + " started again");
            }
        } else {
            st.state = "running";
        }
        r.services.push_back(st);
    }
    return r;
}

std::vector<ServiceStatus> service_status(const Project& project) {
    std::vector<ServiceStatus> out;
    for (const ServiceSpec& s : project.config().services) {
        ServiceStatus st{s.name, service_container(project, s), "", ""};
        auto bad = service_problems(project, s);
        if (!bad.empty()) {
            st.state = "invalid";
            st.error = join(bad, "; ");
        } else if (auto now = inspect(project, st.container)) {
            st.state = now->first;
            if (now->second != spec_hash(project, s)) st.error = "its definition changed: recreated on the next run";
        } else {
            st.state = "missing";
            for (const auto& [inside, rel] : s.files)
                if (!fs::exists(project.dir() / rel)) {
                    st.state = "waiting";
                    st.error = "for " + rel + " (it does not exist yet)";
                }
        }
        out.push_back(st);
    }
    return out;
}

ServicesResult stop_services(const Project& project, bool purge) {
    ServicesResult r;
    for (const ServiceSpec& s : project.config().services) {
        Out o = podman(project, {"rm", "-f", service_container(project, s)}, 120);
        if (o.code == 0) r.notes.push_back("service " + s.name + " stopped and removed");
        if (purge && !s.data.empty()) {
            Out v = podman(project, {"volume", "rm", "-f", volume_name(project, s)}, 120);
            if (v.code == 0) r.notes.push_back("service " + s.name + ": its data removed");
        }
    }
    return r;
}

ServiceAnswer service_action(const Project& project, const std::string& action, const std::string& name, int lines) {
    ServiceAnswer a;
    if (action == "status" || name.empty()) {
        std::string t;
        for (const ServiceStatus& s : service_status(project)) {
            const ServiceSpec* spec = find_service(project, s.name);
            t += "- " + s.name + " (" + (spec ? spec->image : "") + "): " + s.state + (s.error.empty() ? "" : ", " + s.error) +
                 "\n";
        }
        a.ok = true;
        a.text = t.empty() ? "The project has no services." : t;
        return a;
    }
    const ServiceSpec* s = find_service(project, name);
    if (!s) {
        std::string names;
        for (const auto& x : project.config().services) names += (names.empty() ? "" : ", ") + x.name;
        a.text = "no service " + name + (names.empty() ? "; the project has none" : "; the services: " + names);
        return a;
    }
    const std::string c = service_container(project, *s);
    // Not running: say why, not only a state (a file it waits for, a start that failed).
    auto not_running = [&]() -> std::optional<ServiceAnswer> {
        for (const ServiceStatus& st : service_status(project))
            if (st.name == name && st.state != "running") {
                ServiceAnswer w;
                w.text = name + " is not running (" + st.state + ")" + (st.error.empty() ? "" : ": " + st.error) + ".";
                if (st.state == "missing")
                    w.text += " It starts with the next round, or with service_restart; if it does not, report a "
                              "blocker to the operator.";
                return w;
            }
        return std::nullopt;
    };
    if (action == "logs") {
        if (auto w = not_running(); w && w->text.find("(waiting)") != std::string::npos) return *w;
        Out o = podman(project, {"logs", "--tail", std::to_string(std::clamp(lines, 1, 500)), c}, 60);
        a.ok = o.code == 0;
        a.text = a.ok ? (o.lines.empty() ? "(no output)" : tail(o, 500)) : "no logs: " + tail(o);
        if (!a.ok)
            if (auto w = not_running()) a.text += "\n" + w->text;
        return a;
    }
    if (action == "restart") {
        Out o = podman(project, {"restart", c}, 120);
        if (o.code != 0) {
            // Not there (or never started): start it as defined.
            ServicesResult e = ensure_services(project);
            for (const auto& st : e.services)
                if (st.name == name) {
                    a.ok = st.state == "running";
                    a.text = a.ok ? name + " is running"
                                  : name + " did not start (" + st.state + "): " + (st.error.empty() ? "no reason given" : st.error);
                }
            return a;
        }
        a.ok = true;
        a.text = name + " restarted";
        return a;
    }
    if (action == "reset") {
        podman(project, {"rm", "-f", c}, 120);
        if (!s->data.empty()) podman(project, {"volume", "rm", "-f", volume_name(project, *s)}, 120);
        ServicesResult e = ensure_services(project);
        for (const auto& st : e.services)
            if (st.name == name) {
                a.ok = st.state == "running";
                a.text = a.ok ? name + " was reset: its data is gone, it runs fresh" : name + " did not start: " + st.error;
            }
        return a;
    }
    a.text = "unknown action " + action + " (status, logs, restart, reset)";
    return a;
}

std::map<std::string, std::string> service_agent_env(const Project& project) {
    std::map<std::string, std::string> env;
    for (const ServiceSpec& s : project.config().services)
        for (const auto& [k, v] : s.agent_env) env[k] = v;
    return env;
}

std::string services_brief(const Project& project, bool tools) {
    const auto& all = project.config().services;
    if (all.empty()) return "";
    std::string t = "The project's services run outside your sandbox; myteam keeps them running. Use them; do not\n"
                    "start containers yourself (podman and docker do not work here).\n\n";
    for (const ServiceSpec& s : all) {
        t += "- " + s.name + ": " + s.image;
        if (!s.description.empty()) t += ". " + s.description;
        t += "\n";
        for (const auto& p : s.ports) t += "  - on " + loopback_port(p) + "\n";
        if (s.network == "host") t += "  - on the host's network\n";
        for (const auto& [inside, rel] : s.files) {
            t += "  - its " + inside + " is the project's " + rel + " (read-only)\n";
            if (!fs::exists(project.dir() / rel))
                t += "  - it starts once " + rel + " exists: whatever needs this service waits for the task that "
                     "writes it\n";
        }
        if (!s.data.empty()) t += "  - its data is kept across restarts\n";
    }
    auto env = service_agent_env(project);
    if (!env.empty()) {
        t += "\nIn your environment:\n";
        for (const auto& [k, v] : env) t += "- " + k + "=" + v + "\n";
    }
    if (tools)
        t += "\nThe tools service_status, service_logs, service_restart (after you change its files) and\n"
             "service_reset (its data removed: a fresh database) work on them.\n";
    t += "A new service, or a change to one (image, ports, mounts), is the operator's: report a blocker to the\n"
         "operator with what you need and why.\n";
    return t;
}

Json services_json(const Project& project) {
    Json list = Json::array();
    for (const ServiceStatus& s : service_status(project)) {
        const ServiceSpec* spec = find_service(project, s.name);
        Json j = Json::object();
        j.set("name", s.name);
        j.set("container", s.container);
        j.set("image", spec ? spec->image : "");
        j.set("state", s.state);
        j.set("error", s.error);
        Json ports = Json::array();
        if (spec)
            for (const auto& p : spec->ports) ports.push_back(loopback_port(p));
        j.set("ports", ports);
        list.push_back(j);
    }
    Json j = Json::object();
    j.set("services", list);
    return j;
}

}  // namespace myteam
