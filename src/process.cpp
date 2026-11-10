#include "myteam/process.hpp"

#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstring>
#include <fcntl.h>
#include <poll.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>

extern char** environ;

namespace myteam {

namespace {

void write_all(int fd, std::string_view data) {
    while (!data.empty()) {
        ssize_t n = ::write(fd, data.data(), data.size());
        if (n < 0) {
            if (errno == EINTR) continue;
            return;
        }
        data.remove_prefix(static_cast<std::size_t>(n));
    }
}

}  // namespace

ProcessResult run_process(const ProcessOptions& o, const std::function<void(std::string_view)>& on_line) {
    ProcessResult r;
    if (o.argv.empty()) {
        r.error = "empty command";
        return r;
    }
    // stdin is an in-memory file, so a long prompt can never block against
    // the child's output. err_pipe only carries exec failures.
    int out_pipe[2], err_pipe[2];
    if (::pipe2(out_pipe, O_CLOEXEC) || ::pipe2(err_pipe, O_CLOEXEC)) {
        r.error = std::string("pipe: ") + std::strerror(errno);
        return r;
    }
    int in_fd = ::memfd_create("myteam-stdin", MFD_CLOEXEC);
    if (in_fd < 0) {
        r.error = std::string("memfd_create: ") + std::strerror(errno);
        return r;
    }
    write_all(in_fd, o.stdin_text);
    ::lseek(in_fd, 0, SEEK_SET);

    // Prepare everything the child needs before fork.
    std::vector<std::string> env_strings;
    for (char** e = environ; *e; ++e) {
        std::string_view kv(*e);
        auto eq = kv.find('=');
        if (eq != std::string_view::npos && o.env.count(std::string(kv.substr(0, eq)))) continue;
        env_strings.emplace_back(kv);
    }
    for (const auto& [k, v] : o.env) env_strings.push_back(k + "=" + v);
    std::vector<char*> envp, argv;
    for (auto& s : env_strings) envp.push_back(s.data());
    envp.push_back(nullptr);
    std::vector<std::string> args = o.argv;
    for (auto& s : args) argv.push_back(s.data());
    argv.push_back(nullptr);
    std::string cwd = o.cwd.string();
    int err_fd = -1;
    if (!o.stderr_path.empty()) err_fd = ::open(o.stderr_path.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    if (err_fd < 0) err_fd = ::open("/dev/null", O_WRONLY | O_CLOEXEC);

    pid_t pid = ::fork();
    if (pid < 0) {
        r.error = std::string("fork: ") + std::strerror(errno);
        return r;
    }
    if (pid == 0) {
        ::setpgid(0, 0);
        ::dup2(in_fd, 0);
        ::dup2(out_pipe[1], 1);
        ::dup2(err_fd, 2);
        if (!cwd.empty() && ::chdir(cwd.c_str()) != 0) {
            const char msg[] = "cannot change to the work directory\n";
            (void)!::write(2, msg, sizeof msg - 1);
            ::_exit(126);
        }
        ::execvpe(argv[0], argv.data(), envp.data());
        // Report the failure on the error pipe for the parent.
        int e = errno;
        (void)!::write(err_pipe[1], &e, sizeof e);
        ::_exit(127);
    }
    ::setpgid(pid, pid);
    ::close(in_fd);
    ::close(out_pipe[1]);
    ::close(err_pipe[1]);
    if (err_fd >= 0) ::close(err_fd);

    using clock = std::chrono::steady_clock;
    const auto start = clock::now();
    bool term_sent = false;
    clock::time_point term_time;
    std::string buf;
    int exec_errno = 0;
    bool out_open = true, err_open = true;
    char chunk[65536];
    while (out_open || err_open) {
        pollfd fds[2];
        int n = 0;
        if (out_open) fds[n++] = {out_pipe[0], POLLIN, 0};
        if (err_open) fds[n++] = {err_pipe[0], POLLIN, 0};
        int rc = ::poll(fds, static_cast<nfds_t>(n), 500);
        if (rc < 0 && errno != EINTR) break;
        for (int i = 0; i < n && rc > 0; ++i) {
            if (!(fds[i].revents & (POLLIN | POLLHUP | POLLERR))) continue;
            ssize_t got = ::read(fds[i].fd, chunk, sizeof chunk);
            if (got <= 0) {
                if (got < 0 && errno == EINTR) continue;
                (fds[i].fd == out_pipe[0] ? out_open : err_open) = false;
                continue;
            }
            if (fds[i].fd == err_pipe[0]) {
                if (got == sizeof(int)) std::memcpy(&exec_errno, chunk, sizeof(int));
                continue;
            }
            buf.append(chunk, static_cast<std::size_t>(got));
            std::size_t nl;
            while ((nl = buf.find('\n')) != std::string::npos) {
                std::string_view line(buf.data(), nl);
                if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
                if (on_line) on_line(line);
                buf.erase(0, nl + 1);
            }
        }
        if (o.timeout_seconds > 0) {
            auto now = clock::now();
            if (!term_sent && now - start > std::chrono::seconds(o.timeout_seconds)) {
                ::kill(-pid, SIGTERM);
                term_sent = true;
                term_time = now;
                r.timed_out = true;
            } else if (term_sent && now - term_time > std::chrono::seconds(5)) {
                ::kill(-pid, SIGKILL);
            }
        }
    }
    if (!buf.empty() && on_line) on_line(buf);
    ::close(out_pipe[0]);
    ::close(err_pipe[0]);

    int status = 0;
    while (::waitpid(pid, &status, 0) < 0 && errno == EINTR) {
    }
    if (WIFEXITED(status))
        r.exit_code = WEXITSTATUS(status);
    else if (WIFSIGNALED(status))
        r.exit_code = 128 + WTERMSIG(status);
    if (exec_errno) {
        r.exit_code = -1;
        r.error = "cannot run " + o.argv[0] + ": " + std::strerror(exec_errno);
    }
    return r;
}

}  // namespace myteam
