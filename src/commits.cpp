#include "myteam/commits.hpp"

#include <chrono>
#include <unistd.h>

#include "myteam/services.hpp"
#include "myteam/util.hpp"

namespace myteam {

namespace fs = std::filesystem;
using taskml::Json;

namespace {

Json strings(const std::vector<std::string>& v) {
    Json a = Json::array();
    for (const auto& s : v) a.push_back(s);
    return a;
}
std::vector<std::string> strings(const Json* j) {
    std::vector<std::string> out;
    if (j && j->is_array())
        for (const Json& x : j->as_array())
            if (x.is_string()) out.push_back(x.as_string());
    return out;
}
std::string str(const Json& j, const char* k) {
    const Json* v = j.find(k);
    return v && v->is_string() ? v->as_string() : std::string();
}

constexpr const char* kRequest = ".request.json";
constexpr const char* kResponse = ".response.json";

}  // namespace

Json commit_to_json(const RunCommit& c) {
    Json j = Json::object();
    j.set("ok", c.ok);
    j.set("commit", c.commit);
    j.set("branch", c.branch);
    j.set("summary", c.summary);
    j.set("message", c.message);
    j.set("files", strings(c.files));
    j.set("left_out", strings(c.left_out));
    j.set("hooks", strings(c.hooks));
    j.set("hook_changed", strings(c.hook_changed));
    j.set("error", c.error);
    return j;
}

RunCommit commit_from_json(const Json& j) {
    RunCommit c;
    const Json* ok = j.find("ok");
    c.ok = ok && ok->is_bool() && ok->as_bool();
    c.commit = str(j, "commit");
    c.branch = str(j, "branch");
    c.summary = str(j, "summary");
    c.message = str(j, "message");
    c.files = strings(j.find("files"));
    c.left_out = strings(j.find("left_out"));
    c.hooks = strings(j.find("hooks"));
    c.hook_changed = strings(j.find("hook_changed"));
    c.error = str(j, "error");
    return c;
}

Json commit_request_to_json(const CommitRequest& r) {
    Json j = Json::object();
    j.set("message", r.message);
    j.set("paths", strings(r.paths));
    return j;
}

CommitRequest commit_request_from_json(const Json& j) {
    return {str(j, "message"), strings(j.find("paths"))};
}

fs::path commits_dir(const fs::path& run_dir) { return run_dir / "commits"; }

CommitBroker::CommitBroker(const Project& project, std::optional<Snapshot> before, CommitIdentity who, fs::path run_dir)
    : project_(project), before_(std::move(before)), who_(std::move(who)), dir_(commits_dir(run_dir)) {}

CommitBroker::~CommitBroker() { stop(); }

void CommitBroker::start() {
    std::error_code ec;
    fs::create_directories(dir_, ec);
    thread_ = std::thread([this] {
        std::unique_lock<std::mutex> lock(mu_);
        while (!stopping_) {
            lock.unlock();
            poll();
            lock.lock();
            cv_.wait_for(lock, std::chrono::milliseconds(150), [this] { return stopping_; });
        }
    });
}

void CommitBroker::stop() {
    {
        std::lock_guard<std::mutex> lock(mu_);
        if (stopping_ && !thread_.joinable()) return;
        stopping_ = true;
    }
    cv_.notify_all();
    if (thread_.joinable()) thread_.join();
    poll();
}

void CommitBroker::poll() {
    std::error_code ec;
    std::vector<fs::path> waiting;
    for (const auto& e : fs::directory_iterator(dir_, ec)) {
        std::string name = e.path().filename().string();
        if (name.size() <= std::string(kRequest).size() || name.substr(name.size() - std::string(kRequest).size()) != kRequest)
            continue;
        std::string id = name.substr(0, name.size() - std::string(kRequest).size());
        if (!fs::exists(dir_ / (id + kResponse))) waiting.push_back(e.path());
    }
    std::sort(waiting.begin(), waiting.end());
    for (const fs::path& req_path : waiting) {
        std::string name = req_path.filename().string();
        std::string id = name.substr(0, name.size() - std::string(kRequest).size());
        RunCommit result;
        try {
            Json j = Json::parse(read_file(req_path));
            if (const Json* sv = j.find("service"); sv && sv->is_string()) {
                // A service operation: answered as text.
                const Json* ln = j.find("lines");
                ServiceAnswer a = service_action(project_, sv->as_string(), str(j, "name"),
                                                 ln && ln->is_number() ? static_cast<int>(ln->as_number()) : 80);
                Json r = Json::object();
                r.set("ok", a.ok);
                r.set("text", a.text);
                write_file_atomic(dir_ / (id + kResponse), r.dump(2) + "\n");
                continue;
            }
            CommitRequest req = commit_request_from_json(j);
            if (!before_) result.error = "commits are not on for this run (no git repository, or commits: off)";
            else result = commit_run_work(project_, project_.config().guard, *before_, req, who_, dir_);
        } catch (const std::exception& e) {
            result.error = std::string("the request could not be read: ") + e.what();
        }
        write_file_atomic(dir_ / (id + kResponse), commit_to_json(result).dump(2) + "\n");
        if (result.ok) {
            std::lock_guard<std::mutex> lock(mu_);
            commits_.push_back(result);
        }
    }
}

std::vector<RunCommit> CommitBroker::commits() const {
    std::lock_guard<std::mutex> lock(mu_);
    return commits_;
}

namespace {
// Write a request; wait for its answer (nullopt on timeout).
std::optional<Json> ask_myteam(const fs::path& run_dir, const Json& req, int timeout_seconds) {
    static std::atomic<int> counter{0};
    const fs::path dir = commits_dir(run_dir);
    std::error_code ec;
    fs::create_directories(dir, ec);
    const std::string id = file_timestamp() + "-" + std::to_string(::getpid()) + "-" + std::to_string(++counter);
    write_file_atomic(dir / (id + kRequest), req.dump(2) + "\n");
    const fs::path resp = dir / (id + kResponse);
    const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(timeout_seconds);
    while (std::chrono::steady_clock::now() < until) {
        if (auto text = try_read_file(resp)) {
            try {
                return Json::parse(*text);
            } catch (const std::exception&) {
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    return std::nullopt;
}
}  // namespace

RunCommit request_commit(const fs::path& run_dir, const CommitRequest& req, int timeout_seconds) {
    // (A hook may take its time: the default waits 15 minutes.)
    if (auto j = ask_myteam(run_dir, commit_request_to_json(req), timeout_seconds)) return commit_from_json(*j);
    RunCommit r;
    r.error = "myteam did not answer the commit request (is this a task run?); nothing was committed";
    return r;
}

ServiceAnswer request_service(const fs::path& run_dir, const std::string& action, const std::string& name, int lines,
                              int timeout_seconds) {
    Json req = Json::object();
    req.set("service", action);
    req.set("name", name);
    req.set("lines", lines);
    ServiceAnswer a;
    if (auto j = ask_myteam(run_dir, req, timeout_seconds)) {
        const Json* ok = j->find("ok");
        a.ok = ok && ok->is_bool() && ok->as_bool();
        a.text = str(*j, "text");
        return a;
    }
    a.text = "myteam did not answer (is this a task run?)";
    return a;
}

}  // namespace myteam
