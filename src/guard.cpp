#include "myteam/guard.hpp"

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <istream>
#include <map>
#include <optional>
#include <ostream>
#include <regex>
#include <set>

#include "myteam/util.hpp"

namespace myteam {

namespace fs = std::filesystem;
using taskml::Json;

// ---------------------------------------------------------------------------
// Policy
// ---------------------------------------------------------------------------

GuardPolicy guard_policy(const Project& project) { return project.config().guard; }

std::vector<std::string> protected_globs(const Project& project, const GuardPolicy& policy) {
    std::vector<std::string> g = {project.config().plan,
                                  project.config().changelog,
                                  ".myteam/**",
                                  ".git/**",
                                  ".github/workflows/**",
                                  ".gitlab-ci.yml",
                                  ".env",
                                  ".env.*",
                                  "**/.env",
                                  "**/*.pem",
                                  "**/*.key",
                                  "**/id_rsa*",
                                  "**/id_ed25519*"};
    if (!project.config().backlog.empty()) g.push_back(project.config().backlog);
    g.insert(g.end(), policy.protect.begin(), policy.protect.end());
    for (const auto& r : project.config().readonly) {
        g.push_back(r);
        g.push_back(r + "/**");
    }
    // The brief is the team's to keep up to date, though it lives in .myteam/.
    if (std::string b = writable_brief(project); !b.empty()) g.push_back("!" + b);
    // What the project took out (CI, say). Not what myteam itself depends on.
    for (const auto& u : policy.unprotect) {
        std::string x = u;
        while (starts_with(x, "./")) x.erase(0, 2);
        if (x.empty() || starts_with(x, ".myteam") || starts_with(x, ".git/") || x == ".git" || x == "**" ||
            x == "*" || glob_match(x, project.config().plan) || glob_match(x, project.config().changelog))
            continue;
        g.push_back("!" + x);
    }
    return g;
}

std::string writable_brief(const Project& project) {
    std::error_code ec;
    const fs::path dir = fs::weakly_canonical(project.dir(), ec);
    const fs::path brief = fs::weakly_canonical(project.brief_path(), ec);
    std::string rel = brief.lexically_relative(dir).generic_string();
    if (rel.empty() || rel == "." || starts_with(rel, "..") || starts_with(rel, project.customer_rel()) ||
        starts_with(rel, ".git/") || rel == project.config().plan || rel == project.config().changelog ||
        rel == project.config().backlog || starts_with(rel, ".myteam/runs") || rel == ".myteam/config.yaml")
        return "";
    return rel;
}

bool path_protected(const std::vector<std::string>& globs, std::string_view rel) {
    bool hit = false;
    for (const auto& g : globs) {
        if (!g.empty() && g[0] == '!') {
            if (glob_match(std::string_view(g).substr(1), rel)) return false;
        } else if (!hit && glob_match(g, rel)) {
            hit = true;
        }
    }
    return hit;
}

bool glob_match(std::string_view p, std::string_view s) {
    // Iterative matcher with backtracking for * and **.
    std::size_t pi = 0, si = 0;
    std::size_t star_p = std::string_view::npos, star_s = 0;
    bool star_any = false;  // the last star was **
    while (si < s.size()) {
        if (pi < p.size() && p[pi] == '*') {
            bool dbl = pi + 1 < p.size() && p[pi + 1] == '*';
            star_any = dbl;
            pi += dbl ? 2 : 1;
            if (dbl && pi < p.size() && p[pi] == '/') {
                // "**/" also matches nothing.
                if (glob_match(p.substr(pi + 1), s.substr(si))) return true;
            }
            star_p = pi;
            star_s = si;
            continue;
        }
        if (pi < p.size() && (p[pi] == '?' ? s[si] != '/' : p[pi] == s[si])) {
            ++pi;
            ++si;
            continue;
        }
        if (star_p != std::string_view::npos && (star_any || s[star_s] != '/')) {
            pi = star_p;
            si = ++star_s;
            continue;
        }
        return false;
    }
    while (pi < p.size() && p[pi] == '*') ++pi;
    return pi == p.size();
}

namespace {

fs::path home_dir() {
    const char* h = std::getenv("HOME");
    return h ? fs::path(h) : fs::path("/");
}

fs::path normal(const fs::path& p) {
    std::error_code ec;
    fs::path w = fs::weakly_canonical(p, ec);
    return (ec ? p : w).lexically_normal();
}


bool strictly_under(const fs::path& path, const fs::path& dir) {
    auto rel = path.lexically_relative(dir);
    if (rel.empty() || rel == ".") return false;
    auto first = *rel.begin();
    return first != "..";
}

fs::path resolve(const GuardContext& ctx, std::string_view word, const fs::path& cwd) {
    std::string w(word);
    if (w == "~") return normal(home_dir());
    if (starts_with(w, "~/")) return normal(home_dir() / w.substr(2));
    if (starts_with(w, "$HOME")) return normal(home_dir().string() + w.substr(5));
    fs::path p(w);
    if (p.is_relative()) p = cwd / p;
    (void)ctx;
    return normal(p);
}

}  // namespace

GuardContext guard_context(const Project& project, const GuardPolicy& policy, const fs::path& cwd) {
    GuardContext c;
    c.project = normal(project.dir());
    c.cwd = cwd.empty() ? c.project : normal(cwd);
    c.protect = protected_globs(project, policy);
    c.writable.push_back(normal(fs::temp_directory_path()));
    c.writable.push_back("/tmp");
    if (const char* t = std::getenv("TMPDIR"); t && *t) c.writable.push_back(normal(t));
    for (const auto& a : policy.allow_paths) c.writable.push_back(normal(a));
    for (const auto& w : project.write_paths()) c.writable.push_back(normal(w));
    c.policy = &policy;
    c.commit_tool = project.config().mcp && project.config().commits == "tool";
    return c;
}

namespace {

GuardDecision deny(std::string why) { return {false, std::move(why)}; }
const GuardDecision kAllow{true, ""};

std::string protected_reason(const std::string& rel) {
    if (starts_with(rel, ".myteam/")) return "it is myteam's state";
    if (starts_with(rel, ".git/")) return "it is git's own data";
    if (rel.find("workflows/") != std::string::npos || rel == ".gitlab-ci.yml") return "it is the CI configuration";
    if (rel.find(".env") != std::string::npos || rel.find(".pem") != std::string::npos ||
        rel.find(".key") != std::string::npos || rel.find("id_") != std::string::npos)
        return "it may hold secrets";
    return "myteam writes it: report your status, handoffs and blockers instead";
}

// Where writes go: the project and the directories besides it, each once.
std::string allowed_dirs(const GuardContext& ctx) {
    std::vector<std::string> d{ctx.project.string()};
    for (const auto& w : ctx.writable)
        if (std::find(d.begin(), d.end(), w.string()) == d.end()) d.push_back(w.string());
    std::string out;
    for (std::size_t i = 0; i < d.size(); ++i) out += (i ? ", " : "") + d[i];
    return "; writes go to " + out;
}

GuardDecision write_at(const GuardContext& ctx, const fs::path& p) {
    static const char* devices[] = {"/dev/null", "/dev/stdout", "/dev/stderr", "/dev/tty", "/dev/zero"};
    for (const char* d : devices)
        if (p == d) return kAllow;
    if (p == ctx.project || strictly_under(p, ctx.project)) {
        std::string rel = p == ctx.project ? "." : p.lexically_relative(ctx.project).generic_string();
        if (path_protected(ctx.protect, rel)) return deny(rel + " is protected (" + protected_reason(rel) + ")");
        return kAllow;
    }
    for (const auto& w : ctx.writable)
        if (p == w || strictly_under(p, w)) return kAllow;
    return deny(p.string() + " is outside the project" + allowed_dirs(ctx));
}

}  // namespace

GuardDecision check_write(const GuardContext& ctx, std::string_view path) {
    return write_at(ctx, resolve(ctx, path, ctx.cwd));
}

GuardDecision check_read(const GuardContext& ctx, std::string_view path) {
    const fs::path p = resolve(ctx, path, ctx.cwd);
    for (const fs::path& h : ctx.hidden)
        if (p == h || strictly_under(p, h))
            return deny(p.string() + " is not for you to read: your task and what it needs are in your prompt");
    const fs::path h = normal(home_dir());
    static const char* secrets[] = {".ssh",        ".aws",          ".gnupg",          ".config/gh",
                                    ".netrc",      ".git-credentials", ".docker/config.json", ".kube",
                                    ".config/gcloud", ".pypirc",    ".npmrc",          ".codex/auth.json",
                                    ".claude/.credentials.json"};
    for (const char* s : secrets) {
        fs::path d = h / s;
        if (p == d || strictly_under(p, d)) return deny(p.string() + " holds credentials");
    }
    return kAllow;
}

// ---------------------------------------------------------------------------
// Shell commands
// ---------------------------------------------------------------------------

namespace {

struct SimpleCmd {
    std::vector<std::string> words;
    std::vector<std::string> redirects;  ///< output targets
    bool piped = false;                  ///< its stdin is the previous command's stdout
    std::vector<std::string> inner;      ///< $(...) and `...` contents (and a shell's heredoc)
};

// The command a heredoc feeds runs its body as a script.
bool feeds_shell(const std::string& w) {
    std::string b = w.substr(w.rfind('/') == std::string::npos ? 0 : w.rfind('/') + 1);
    return b == "sh" || b == "bash" || b == "zsh" || b == "dash" || b == "ksh" || b == "fish";
}

std::vector<SimpleCmd> parse(std::string_view s) {
    std::vector<SimpleCmd> out(1);
    std::string word;
    bool in_word = false, redirect_next = false, skip_next = false;
    auto end_word = [&] {
        if (!in_word) return;
        if (redirect_next) out.back().redirects.push_back(word);
        else if (!skip_next) out.back().words.push_back(word);
        redirect_next = skip_next = false;
        word.clear();
        in_word = false;
    };
    auto end_cmd = [&](bool pipe) {
        end_word();
        out.emplace_back();
        out.back().piped = pipe;
    };
    // Heredocs waiting for the end of their line: the command they feed, the
    // delimiter, and whether leading tabs are stripped (<<-).
    struct Heredoc {
        std::size_t cmd;
        std::string delim;
        bool dash;
    };
    std::vector<Heredoc> heredocs;
    // At a newline: skip the bodies of the line's heredocs (data, not
    // commands), and keep a shell's as a script to check.
    auto skip_bodies = [&](std::size_t& i) {
        std::size_t pos = i + 1;
        for (const Heredoc& h : heredocs) {
            std::string body;
            while (pos <= s.size()) {
                std::size_t e = s.find('\n', pos);
                std::string_view line = s.substr(pos, (e == std::string_view::npos ? s.size() : e) - pos);
                std::string_view cmp = line;
                if (h.dash)
                    while (!cmp.empty() && cmp.front() == '\t') cmp.remove_prefix(1);
                pos = e == std::string_view::npos ? s.size() + 1 : e + 1;
                if (cmp == h.delim) break;
                body += std::string(line) + "\n";
            }
            if (h.cmd < out.size() && !out[h.cmd].words.empty() && feeds_shell(out[h.cmd].words[0]))
                out[h.cmd].inner.push_back(body);
        }
        heredocs.clear();
        i = pos - 1;  // the loop's ++i lands on the line after the last delimiter
    };
    for (std::size_t i = 0; i < s.size(); ++i) {
        char c = s[i];
        if (c == '\'') {
            std::size_t e = s.find('\'', i + 1);
            if (e == std::string_view::npos) e = s.size();
            word += s.substr(i + 1, e - i - 1);
            in_word = true;
            i = e;
        } else if (c == '"') {
            std::size_t j = i + 1;
            while (j < s.size() && s[j] != '"') {
                if (s[j] == '\\' && j + 1 < s.size()) {
                    word += s[j + 1];
                    j += 2;
                    continue;
                }
                if (s[j] == '$' && j + 1 < s.size() && s[j + 1] == '(') {
                    int depth = 1;
                    std::size_t k = j + 2;
                    while (k < s.size() && depth) {
                        if (s[k] == '(') ++depth;
                        else if (s[k] == ')') --depth;
                        ++k;
                    }
                    out.back().inner.emplace_back(s.substr(j + 2, k - j - 3));
                }
                word += s[j++];
            }
            in_word = true;
            i = j;
        } else if (c == '\\' && i + 1 < s.size()) {
            if (s[i + 1] != '\n') word += s[i + 1];
            in_word = true;
            ++i;
        } else if (c == '$' && i + 1 < s.size() && s[i + 1] == '(') {
            int depth = 1;
            std::size_t k = i + 2;
            while (k < s.size() && depth) {
                if (s[k] == '(') ++depth;
                else if (s[k] == ')') --depth;
                ++k;
            }
            out.back().inner.emplace_back(s.substr(i + 2, k - i - 3));
            word += std::string(s.substr(i, k - i));
            in_word = true;
            i = k - 1;
        } else if (c == '`') {
            std::size_t e = s.find('`', i + 1);
            if (e == std::string_view::npos) e = s.size();
            out.back().inner.emplace_back(s.substr(i + 1, e - i - 1));
            in_word = true;
            i = e;
        } else if (c == ' ' || c == '\t') {
            end_word();
        } else if (c == '\n') {
            const std::size_t at = i;
            end_cmd(false);
            if (!heredocs.empty()) {
                skip_bodies(i);
                if (i < at) i = at;
            }
        } else if (c == ';') {
            end_cmd(false);
        } else if (c == '&') {
            if (i + 1 < s.size() && s[i + 1] == '&') ++i;
            else if (i + 1 < s.size() && s[i + 1] == '>') {  // &> file
                end_word();
                redirect_next = true;
                ++i;
                if (i + 1 < s.size() && s[i + 1] == '>') ++i;
                continue;
            }
            end_cmd(false);
        } else if (c == '|') {
            if (i + 1 < s.size() && s[i + 1] == '|') {
                ++i;
                end_cmd(false);
            } else {
                end_cmd(true);
            }
        } else if (c == '>') {
            // N> or N>&M: a descriptor number before is not a word.
            if (in_word && std::all_of(word.begin(), word.end(), ::isdigit)) {
                word.clear();
                in_word = false;
            }
            end_word();
            if (i + 1 < s.size() && s[i + 1] == '>') ++i;
            if (i + 1 < s.size() && s[i + 1] == '&') {  // >&2
                ++i;
                while (i + 1 < s.size() && std::isdigit(static_cast<unsigned char>(s[i + 1]))) ++i;
                continue;
            }
            redirect_next = true;
        } else if (c == '<') {
            end_word();
            if (i + 1 < s.size() && s[i + 1] == '<' && !(i + 2 < s.size() && s[i + 2] == '<')) {
                // A heredoc: its delimiter (quotes taken off); the body follows the line.
                ++i;
                bool dash = i + 1 < s.size() && s[i + 1] == '-';
                if (dash) ++i;
                std::size_t j = i + 1;
                while (j < s.size() && (s[j] == ' ' || s[j] == '\t')) ++j;
                std::string delim;
                while (j < s.size() && !std::strchr(" \t\n;|&<>()", s[j])) {
                    if (s[j] != '\'' && s[j] != '"' && s[j] != '\\') delim += s[j];
                    ++j;
                }
                if (!delim.empty()) heredocs.push_back({out.size() - 1, delim, dash});
                i = j - 1;
                continue;
            }
            if (i + 2 < s.size() && s[i + 1] == '<' && s[i + 2] == '<') i += 2;  // <<< a here-string
            skip_next = true;
        } else if (c == '(' || c == ')' || c == '{' || c == '}') {
            if (in_word) word += c;
            else end_word();
        } else if (c == '#' && !in_word) {
            std::size_t e = s.find('\n', i);
            i = e == std::string_view::npos ? s.size() : e - 1;
        } else {
            word += c;
            in_word = true;
        }
    }
    end_word();
    out.erase(std::remove_if(out.begin(), out.end(),
                             [](const SimpleCmd& c) { return c.words.empty() && c.redirects.empty() && c.inner.empty(); }),
              out.end());
    return out;
}

std::string base(const std::string& w) {
    auto p = w.rfind('/');
    return p == std::string::npos ? w : w.substr(p + 1);
}

bool has_flag(const std::vector<std::string>& args, std::initializer_list<const char*> longs, std::string_view shorts) {
    for (const auto& a : args) {
        if (a == "--") break;
        for (const char* l : longs)
            if (a == l || starts_with(a, std::string(l) + "=")) return true;
        if (a.size() > 1 && a[0] == '-' && a[1] != '-')
            for (char c : a.substr(1))
                if (shorts.find(c) != std::string_view::npos) return true;
    }
    return false;
}

std::vector<std::string> operands(const std::vector<std::string>& args) {
    std::vector<std::string> out;
    bool ended = false;
    for (const auto& a : args) {
        if (!ended && a == "--") {
            ended = true;
            continue;
        }
        if (!ended && !a.empty() && a[0] == '-') continue;
        out.push_back(a);
    }
    return out;
}

bool is_shell(const std::string& c) {
    return c == "sh" || c == "bash" || c == "zsh" || c == "dash" || c == "ksh" || c == "fish";
}

bool is_interpreter(const std::string& c) {
    return is_shell(c) || c == "python" || c == "python3" || c == "perl" || c == "ruby" || c == "node";
}

bool local_url(const std::vector<std::string>& args) {
    for (const auto& a : args)
        if (a.find("://localhost") != std::string::npos || a.find("://127.0.0.1") != std::string::npos ||
            a.find("://[::1]") != std::string::npos || starts_with(a, "localhost") || starts_with(a, "127.0.0.1"))
            return true;
    return false;
}

const char* kAskOperator = "; if the task needs it, ask the operator with <blocker to=\"operator\">";

struct Checker {
    const GuardContext& ctx;
    fs::path cwd;
    int depth = 0;
    bool venv = false;  ///< a virtualenv in the project was activated earlier in the line
    std::map<std::string, std::string> vars;  ///< NAME=value set earlier in the line (J=/tmp/x; rm $J)

    // $NAME and ${NAME} of the variables set earlier; others stay as they are.
    std::string expand(const std::string& w) const {
        if (w.find('$') == std::string::npos || vars.empty()) return w;
        std::string out;
        for (std::size_t i = 0; i < w.size(); ++i) {
            if (w[i] != '$' || i + 1 >= w.size()) {
                out += w[i];
                continue;
            }
            bool brace = w[i + 1] == '{';
            std::size_t s = i + (brace ? 2 : 1), e = s;
            while (e < w.size() && (std::isalnum(static_cast<unsigned char>(w[e])) || w[e] == '_')) ++e;
            std::string name = w.substr(s, e - s);
            auto it = vars.find(name);
            if (name.empty() || it == vars.end() || (brace && (e >= w.size() || w[e] != '}'))) {
                out += w[i];
                continue;
            }
            out += it->second;
            i = brace ? e : e - 1;
        }
        return out;
    }

    // A command that only sets variables (or exports them): remember them.
    void remember(const SimpleCmd& c) {
        std::size_t from = (!c.words.empty() && (c.words[0] == "export" || c.words[0] == "local" ||
                                                 c.words[0] == "declare" || c.words[0] == "readonly"))
                               ? 1
                               : 0;
        static const std::regex assign(R"(^([A-Za-z_][A-Za-z0-9_]*)=(.*)$)");
        for (std::size_t i = from; i < c.words.size(); ++i) {
            std::smatch m;
            if (!std::regex_match(c.words[i], m, assign)) return;  // a command with a prefix: not the shell's
        }
        for (std::size_t i = from; i < c.words.size(); ++i) {
            std::smatch m;
            std::regex_match(c.words[i], m, assign);
            const std::string v = m[2].str();
            if (v.find("$(") != std::string::npos || v.find('`') != std::string::npos) vars.erase(m[1].str());
            else vars[m[1].str()] = v;
        }
    }

    GuardDecision writes(const std::vector<std::string>& paths) {
        for (const auto& p : paths) {
            if (p.find_first_of("*?") != std::string::npos) {
                // A glob: what it can reach is decided by its directory.
                fs::path dir = resolve(ctx, p, cwd).parent_path();
                if (auto d = write_at(ctx, dir / "x"); !d.allow) return d;
                continue;
            }
            if (auto d = write_at(ctx, resolve(ctx, p, cwd)); !d.allow) return d;
        }
        return kAllow;
    }

    GuardDecision reads(const std::vector<std::string>& words) {
        for (const auto& w : words)
            if (w.find('/') != std::string::npos || starts_with(w, "~") || starts_with(w, "$HOME"))
                if (auto d = check_read(GuardContext{ctx.project, cwd, ctx.protect, ctx.writable, ctx.policy, ctx.commit_tool, ctx.hidden}, w); !d.allow)
                    return d;
        return kAllow;
    }

    GuardDecision line(std::string_view text) {
        if (++depth > 6) return deny("the command is nested too deeply to check");
        // The user's own rules first.
        if (ctx.policy) {
            for (const auto& r : ctx.policy->allow_commands) {
                try {
                    if (std::regex_search(std::string(text), std::regex(r))) return kAllow;
                } catch (const std::regex_error&) {
                }
            }
            for (const auto& r : ctx.policy->deny_commands) {
                try {
                    if (std::regex_search(std::string(text), std::regex(r)))
                        return deny("the project's guard refuses commands matching /" + r + "/");
                } catch (const std::regex_error&) {
                }
            }
        }
        if (text.find(":(){") != std::string_view::npos || text.find(":() {") != std::string_view::npos)
            return deny("that is a fork bomb");
        auto cmds = parse(text);
        std::string prev;
        for (const SimpleCmd& raw : cmds) {
            SimpleCmd c = raw;
            for (auto& w : c.words) w = expand(w);
            for (auto& r : c.redirects) r = expand(r);
            remember(c);
            for (const auto& in : c.inner)
                if (auto d = line(in); !d.allow) return d;
            if (auto d = writes(c.redirects); !d.allow) return d;
            if (auto d = reads(c.words); !d.allow) return d;
            if (auto d = command(c, prev); !d.allow) return d;
            prev = c.words.empty() ? "" : base(c.words[0]);
        }
        --depth;
        return kAllow;
    }

    GuardDecision command(const SimpleCmd& sc, const std::string& prev) {
        std::vector<std::string> w = sc.words;
        // Leading assignments and wrappers.
        while (!w.empty()) {
            const std::string& h = w[0];
            if (h.find('=') != std::string::npos && h[0] != '-' && h[0] != '=' &&
                std::isalpha(static_cast<unsigned char>(h[0]))) {
                w.erase(w.begin());
                continue;
            }
            std::string b = base(h);
            if (b == "env" || b == "nice" || b == "nohup" || b == "time" || b == "command" || b == "exec" ||
                b == "stdbuf" || b == "ionice") {
                w.erase(w.begin());
                while (!w.empty() && w[0][0] == '-') w.erase(w.begin());
                continue;
            }
            if (b == "timeout") {
                w.erase(w.begin());
                while (!w.empty() && (w[0][0] == '-' || std::isdigit(static_cast<unsigned char>(w[0][0])))) w.erase(w.begin());
                continue;
            }
            break;
        }
        if (w.empty()) return kAllow;
        const std::string cmd = base(w[0]);
        std::vector<std::string> args(w.begin() + 1, w.end());
        auto ops = operands(args);

        if (cmd == "sudo" || cmd == "su" || cmd == "doas" || cmd == "pkexec" || cmd == "run0")
            return deny("it needs root" + std::string(kAskOperator));
        if (cmd == "podman" || cmd == "docker" || cmd == "podman-compose" || cmd == "docker-compose" || cmd == "nerdctl")
            return deny("containers are myteam's: the project's services run outside the sandbox (see Services in "
                        "your context; the service_* tools). A new service, or a change to one, is the operator's");
        if (cmd == "cd") {
            cwd = ops.empty() ? normal(home_dir()) : resolve(ctx, ops[0], cwd);
            return kAllow;
        }
        // `. .venv/bin/activate`: pip installs into it from here on.
        if ((cmd == "." || cmd == "source") && !args.empty()) {
            fs::path a = resolve(ctx, args[0], cwd);
            if (a.filename() == "activate" && a.parent_path().filename() == "bin" &&
                (a == ctx.project || strictly_under(a, ctx.project)))
                venv = true;
            return kAllow;
        }
        if (is_shell(cmd) || cmd == "eval") {
            // sh -c "...": check what it runs.
            for (std::size_t i = 0; i < args.size(); ++i)
                if ((args[i] == "-c" || (args[i].size() > 1 && args[i][0] == '-' && args[i].find('c') != std::string::npos &&
                                         args[i][1] != '-')) &&
                    i + 1 < args.size())
                    return line(args[i + 1]);
            if (cmd == "eval") {
                std::string joined;
                for (const auto& a : args) joined += a + " ";
                return line(joined);
            }
            if (sc.piped && (prev == "curl" || prev == "wget"))
                return deny("it runs a script straight from the network" + std::string(kAskOperator));
            return kAllow;
        }
        if (sc.piped && (prev == "curl" || prev == "wget") && is_interpreter(cmd))
            return deny("it runs a script straight from the network" + std::string(kAskOperator));
        if (cmd == "xargs") {
            // xargs [opts] cmd args...: the targets come from stdin, unseen.
            std::size_t i = 0;
            while (i < args.size() && args[i][0] == '-') ++i;
            if (i < args.size()) {
                std::string inner = base(args[i]);
                std::vector<std::string> rest(args.begin() + static_cast<long>(i) + 1, args.end());
                if ((inner == "rm" && has_flag(rest, {"--recursive"}, "rR")) || inner == "shred")
                    return deny("xargs " + inner + " deletes paths it does not show; list them, or delete them by name");
                SimpleCmd sub;
                sub.words.assign(args.begin() + static_cast<long>(i), args.end());
                return command(sub, "");
            }
            return kAllow;
        }
        if (cmd == "rm" || cmd == "rmdir" || cmd == "unlink" || cmd == "shred") {
            if (cmd == "shred") return deny("shred destroys data" + std::string(kAskOperator));
            const bool recursive = has_flag(args, {"--recursive"}, "rR");
            for (const auto& o : ops) {
                fs::path p = resolve(ctx, o, cwd);
                if (recursive && (p == ctx.project || p == normal(home_dir()) || p == "/" || o == "*" || o == "/*"))
                    return deny("rm -r " + o + " would delete the whole project or more");
            }
            return writes(ops);
        }
        if (cmd == "mv" || cmd == "cp" || cmd == "ln" || cmd == "install" || cmd == "rsync") {
            if (cmd == "mv") return writes(ops);  // the sources go away too
            if (!ops.empty()) return writes({ops.back()});
            return kAllow;
        }
        if (cmd == "mkdir" || cmd == "touch" || cmd == "truncate" || cmd == "tee" || cmd == "chmod" || cmd == "chown" ||
            cmd == "chgrp") {
            std::vector<std::string> targets = ops;
            if ((cmd == "chmod" || cmd == "chown" || cmd == "chgrp") && !targets.empty()) targets.erase(targets.begin());
            if (cmd == "truncate") {
                targets.clear();
                for (std::size_t i = 0; i < args.size(); ++i) {
                    if (args[i] == "-s" || args[i] == "--size") {
                        ++i;
                        continue;
                    }
                    if (args[i][0] != '-') targets.push_back(args[i]);
                }
            }
            return writes(targets);
        }
        if (cmd == "sed" || cmd == "perl") {
            if (!has_flag(args, {"--in-place"}, "i")) return kAllow;
            // Files: the operands after the script (unless -e gives it).
            bool script_given = has_flag(args, {"--expression"}, "e");
            std::vector<std::string> files;
            bool skipped = script_given;
            for (std::size_t i = 0; i < args.size(); ++i) {
                const auto& a = args[i];
                if (a == "-e" || a == "--expression") {
                    ++i;
                    continue;
                }
                if (a[0] == '-') continue;
                if (!skipped) {
                    skipped = true;
                    continue;
                }
                files.push_back(a);
            }
            return writes(files);
        }
        if (cmd == "dd") {
            for (const auto& a : args)
                if (starts_with(a, "of=")) {
                    if (starts_with(a, "of=/dev/") && a != "of=/dev/null") return deny("dd writes to a device");
                    return writes({a.substr(3)});
                }
            return kAllow;
        }
        if (cmd == "find") {
            bool deletes = false;
            for (std::size_t i = 0; i < args.size(); ++i)
                if (args[i] == "-delete" ||
                    ((args[i] == "-exec" || args[i] == "-execdir") && i + 1 < args.size() &&
                     (base(args[i + 1]) == "rm" || base(args[i + 1]) == "shred")))
                    deletes = true;
            if (!deletes) return kAllow;
            std::vector<std::string> starts;
            for (const auto& a : args) {
                if (!a.empty() && (a[0] == '-' || a[0] == '(' || a[0] == '!')) break;
                starts.push_back(a);
            }
            if (starts.empty()) starts.push_back(".");
            for (const auto& s : starts) {
                fs::path p = resolve(ctx, s, cwd);
                if (auto d = write_at(ctx, p / "x"); !d.allow) return d;
                // From the project root, protected paths are in reach.
                if (p == ctx.project)
                    for (const auto& a : args)
                        if (a == ".git" || a == ".myteam") return deny("find -delete reaches " + a);
            }
            return kAllow;
        }
        if (cmd == "git") return git(args);
        if (cmd == "curl" || cmd == "wget") {
            const bool sends = has_flag(args, {"--data", "--data-binary", "--data-raw", "--data-urlencode", "--form",
                                               "--upload-file", "--post-data", "--post-file", "--json"},
                                        "dFT");
            if (sends && !local_url(args)) return deny("it sends data out of the machine" + std::string(kAskOperator));
            for (std::size_t i = 0; i + 1 < args.size(); ++i)
                if (args[i] == "-o" || args[i] == "--output" || args[i] == "-O" || args[i] == "--output-document")
                    if (args[i] != "-O" || cmd == "wget")
                        if (auto d = writes({args[i + 1]}); !d.allow) return d;
            return kAllow;
        }
        // Installs outside the project.
        if (cmd == "pip" || cmd == "pip3" || ((cmd == "python" || cmd == "python3") && args.size() >= 3 &&
                                              args[0] == "-m" && (args[1] == "pip" || args[1] == "pip3"))) {
            auto sub = cmd.rfind("pip", 0) == 0 ? args : std::vector<std::string>(args.begin() + 2, args.end());
            if (!sub.empty() && sub[0] == "install") {
                if (has_flag(sub, {"--user", "--break-system-packages"}, ""))
                    return deny("it installs into your account, not the project; use the project's virtualenv (.venv)");
                const char* active = std::getenv("VIRTUAL_ENV");
                bool in_project = w[0].find("/.venv/") != std::string::npos || w[0].find("venv/") != std::string::npos ||
                                  (active && *active) || venv;
                if (!in_project && !has_flag(sub, {"--target", "--prefix", "--root"}, "t"))
                    return deny("pip install outside a virtualenv changes the system's Python; create and use .venv "
                                "(python3 -m venv .venv; .venv/bin/pip install ...)");
            }
            return kAllow;
        }
        if (cmd == "npm" || cmd == "pnpm" || cmd == "yarn") {
            // Global changes only: listing or reading global packages (npm ls -g) is fine.
            std::vector<std::string> rest = args;
            if (!rest.empty() && rest[0] == "global") rest.erase(rest.begin());  // yarn global add
            const bool global = has_flag(args, {"--global"}, "g") || (!args.empty() && args[0] == "global");
            static const std::set<std::string> changes = {"install", "i", "add", "remove", "rm", "uninstall", "un",
                                                          "update", "up", "upgrade", "link", "ln", "unlink", "ci"};
            std::string sub;
            for (const auto& x : rest)
                if (!x.empty() && x[0] != '-') {
                    sub = x;
                    break;
                }
            if (global && changes.count(sub)) return deny("it installs globally; install into the project instead");
        }
        if (cmd == "cargo" && !args.empty() && args[0] == "install") return deny("cargo install installs globally");
        if (cmd == "go" && !args.empty() && args[0] == "install") return deny("go install installs globally");
        if (cmd == "gem" && !args.empty() && args[0] == "install") return deny("gem install installs globally");
        static const char* system_pkgs[] = {"dnf", "yum", "apt", "apt-get", "zypper", "pacman", "rpm-ostree",
                                            "flatpak", "snap", "brew", "rpm", "dpkg", "toolbox", "distrobox"};
        for (const char* p : system_pkgs)
            if (cmd == p) return deny("it changes the system's software" + std::string(kAskOperator));
        static const char* system_cmds[] = {"shutdown", "reboot", "poweroff", "halt", "systemctl", "mount", "umount",
                                            "fdisk", "parted", "killall", "crontab", "useradd", "passwd", "iptables",
                                            "nft"};
        for (const char* p : system_cmds)
            if (cmd == p) return deny(cmd + " changes the machine, not the project" + std::string(kAskOperator));
        if (starts_with(cmd, "mkfs")) return deny("it formats a disk");
        if (cmd == "kill" && std::find(args.begin(), args.end(), "-1") != args.end())
            return deny("kill -1 kills every process you own");
        return kAllow;
    }

    GuardDecision git(std::vector<std::string> args) {
        // Global options: -C <dir>, -c <k=v>, --git-dir ...
        fs::path repo = cwd;
        while (!args.empty() && args[0][0] == '-') {
            if ((args[0] == "-C" || args[0] == "-c" || args[0] == "--git-dir" || args[0] == "--work-tree") && args.size() > 1) {
                if (args[0] == "-C") repo = resolve(ctx, args[1], cwd);
                if (args[0] == "--git-dir" || args[0] == "--work-tree") return deny("git with another repository");
                args.erase(args.begin(), args.begin() + 2);
                continue;
            }
            args.erase(args.begin());
        }
        // Reading another repository is harmless (`git -C ../tool rev-parse` to record a
        // released commit); hidden paths are refused as reads anyway.
        static const std::set<std::string> read_only = {"rev-parse", "log", "show", "status", "diff", "describe",
                                                        "ls-files", "ls-tree", "cat-file", "blame", "shortlog",
                                                        "rev-list", "name-rev", "merge-base", "grep"};
        if (!args.empty() && read_only.count(args[0])) return kAllow;
        if (!(repo == ctx.project || strictly_under(repo, ctx.project)) &&
            // A directory the project writes outside it, itself a repository (a tool's worktree,
            // which its team must be able to git status), or one below it.
            std::none_of(ctx.writable.begin(), ctx.writable.end(),
                         [&](const fs::path& w) {
                             // Its own repository (a worktree's .git file), not a folder of another's.
                             return (repo == w || strictly_under(repo, w)) && fs::exists(w / ".git");
                         }))
            return deny("git in " + repo.string() + ", outside the project" + allowed_dirs(ctx));
        if (args.empty()) return kAllow;
        const std::string sub = args[0];
        std::vector<std::string> rest(args.begin() + 1, args.end());
        auto ops = operands(rest);
        const std::string checkpoint = "; myteam keeps a checkpoint of every run, so there is no need";
        if (sub == "push") return deny("git push publishes the work; the operator does that");
        if (sub == "reset" && has_flag(rest, {"--hard", "--merge", "--keep"}, ""))
            return deny("git reset --hard throws work away" + checkpoint);
        if (sub == "clean") return deny("git clean deletes untracked files");
        if (sub == "rebase" || sub == "filter-branch" || sub == "filter-repo" || sub == "replace" || sub == "update-ref" ||
            sub == "reflog" || sub == "gc" || sub == "prune")
            return deny("git " + sub + " rewrites history");
        if (sub == "commit" && has_flag(rest, {"--amend"}, "")) return deny("git commit --amend rewrites history");
        if (ctx.commit_tool && (sub == "commit" || sub == "add" || sub == "stage"))
            return deny("commit with the git_commit tool (mcp__myteam__git_commit): myteam checks your changes and "
                        "commits them; there is no need to git add");
        if (sub == "switch") return deny("switching branches changes the whole work tree under the team");
        if (sub == "checkout") {
            bool files = std::find(rest.begin(), rest.end(), "--") != rest.end();
            if (!files) return deny("git checkout of a branch changes the whole work tree; to restore a file use git restore FILE");
            for (const auto& o : ops)
                if (o == "." || o == "*" || resolve(ctx, o, cwd) == ctx.project) return deny("that discards every change in the project");
            return writes(ops);
        }
        if (sub == "restore") {
            for (const auto& o : ops)
                if (o == "." || o == "*" || o == ":/" || resolve(ctx, o, cwd) == ctx.project)
                    return deny("that discards every change in the project");
            return writes(ops);
        }
        if (sub == "branch" && has_flag(rest, {"--delete", "--move", "--force"}, "dDmMf")) return deny("git branch -d/-m changes branches");
        if (sub == "tag" && has_flag(rest, {"--delete"}, "d")) return deny("git tag -d deletes a tag");
        // The stash stack is one per repository, shared by every worktree and every run: a pop or apply
        // can take another run's entry, and a push can sweep up changes that are not the agent's
        // (protected files, the org's). Only reading it is allowed. Bare `git stash`, `git stash -u`
        // and `save` are a push; `branch` pops; `drop`/`clear` delete stashed work.
        if (sub == "stash") {
            if (!rest.empty() && (rest[0] == "list" || rest[0] == "show")) return kAllow;
            if (!rest.empty() && (rest[0] == "drop" || rest[0] == "clear"))
                return deny("it deletes stashed work; git stash list and git stash show still work");
            return deny("the git stash stack is shared by every worktree and run of this repository, so a pop or "
                        "apply can take someone else's changes, and changes in protected paths are not yours to "
                        "move; only git stash list and git stash show are allowed. To set your own work aside, "
                        "commit it (" + std::string(ctx.commit_tool ? "with the git_commit tool; " : "") +
                        "myteam also keeps a checkpoint of every run)");
        }
        if (sub == "remote" && !rest.empty() && rest[0] != "-v" && rest[0] != "show") return deny("git remote changes where the work goes");
        if (sub == "config" && has_flag(rest, {"--global", "--system"}, "")) return deny("it changes git for the whole account");
        if (sub == "worktree" && !rest.empty() && (rest[0] == "remove" || rest[0] == "prune")) return deny("it deletes a work tree");
        if (sub == "rm" || sub == "mv") return writes(ops);
        return kAllow;
    }
};

}  // namespace

GuardDecision check_command(const GuardContext& ctx, std::string_view command) {
    Checker c{ctx, ctx.cwd};
    return c.line(command);
}

GuardDecision check_tool_call(const GuardContext& ctx, const Json& input) {
    auto str = [](const Json* j, const char* k) {
        const Json* v = j ? j->find(k) : nullptr;
        return v && v->is_string() ? v->as_string() : std::string();
    };
    const std::string tool = str(&input, "tool_name");
    const Json* in = input.find("tool_input");
    if (tool == "Bash") {
        // A command naming a hidden path (cat ../PLAN.md, grep -r x ../.myteam): refused as a read.
        if (!ctx.hidden.empty()) {
            const std::string cmd = str(in, "command");
            std::string word;
            auto check_word = [&](const std::string& w) -> GuardDecision {
                if (w.empty() || w[0] == '-') return kAllow;
                std::string t = w;
                if (auto eq = t.find('='); eq != std::string::npos && t.find('/') > eq) t = t.substr(eq + 1);
                return check_read(ctx, t);
            };
            for (char c : cmd + " ") {
                if (c == ' ' || c == '\t' || c == '\n' || c == ';' || c == '|' || c == '&' || c == '(' || c == ')' ||
                    c == '<' || c == '>' || c == '"' || c == '\'' || c == '`') {
                    if (GuardDecision d = check_word(word); !d.allow) return d;
                    word.clear();
                } else {
                    word += c;
                }
            }
        }
        return check_command(ctx, str(in, "command"));
    }
    if (tool == "Write" || tool == "Edit" || tool == "MultiEdit") return check_write(ctx, str(in, "file_path"));
    if (tool == "NotebookEdit") return check_write(ctx, str(in, "notebook_path"));
    if (tool == "Read") return check_read(ctx, str(in, "file_path"));
    if (tool == "Grep" || tool == "Glob") {
        std::string p = str(in, "path");
        return p.empty() ? kAllow : check_read(ctx, p);
    }
    return kAllow;
}

// ---------------------------------------------------------------------------
// The log and the hook
// ---------------------------------------------------------------------------

void log_guard_block(const fs::path& run_dir, const GuardBlock& b) {
    if (run_dir.empty()) return;
    Json j = Json::object();
    j.set("time", iso_timestamp());
    j.set("tool", b.tool);
    j.set("what", b.what);
    j.set("reason", b.reason);
    fs::create_directories(run_dir);
    append_file(run_dir / "guard.jsonl", j.dump() + "\n");
}

std::vector<GuardBlock> load_guard_blocks(const fs::path& run_dir) {
    std::vector<GuardBlock> out;
    auto text = try_read_file(run_dir / "guard.jsonl");
    if (!text) return out;
    for (const auto& l : split_lines(*text)) {
        if (trim(l).empty()) continue;
        try {
            Json j = Json::parse(l);
            auto s = [&](const char* k) {
                const Json* v = j.find(k);
                return v && v->is_string() ? v->as_string() : std::string();
            };
            out.push_back({s("tool"), s("what"), s("reason")});
        } catch (...) {
        }
    }
    return out;
}

int run_guard_hook(const Project& project, const fs::path& run_dir, std::istream& in, std::ostream& err,
                   const std::vector<std::string>& hidden) {
    std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    Json input;
    try {
        input = Json::parse(text);
    } catch (const std::exception&) {
        err << "myteam guard: could not read the tool call; it is refused\n";
        return 2;
    }
    const GuardPolicy policy = guard_policy(project);
    if (!policy.enabled) return 0;
    std::string cwd;
    if (const Json* c = input.find("cwd"); c && c->is_string()) cwd = c->as_string();
    GuardContext ctx = guard_context(project, policy, cwd.empty() ? project.dir() : fs::path(cwd));
    for (const std::string& h : hidden) ctx.hidden.push_back(fs::weakly_canonical(h));
    GuardDecision d = check_tool_call(ctx, input);
    if (d.allow) return 0;
    std::string tool, what;
    if (const Json* t = input.find("tool_name"); t && t->is_string()) tool = t->as_string();
    if (const Json* ti = input.find("tool_input"))
        for (const char* k : {"command", "file_path", "notebook_path", "path"})
            if (const Json* v = ti->find(k); v && v->is_string()) {
                what = v->as_string();
                break;
            }
    log_guard_block(run_dir, {tool, first_line(what, 300), d.reason});
    err << "myteam guard: refused, " << d.reason
        << ". Do the task another way. If it cannot be done without this, report <blocker to=\"operator\">what you "
           "need and why</blocker>.\n";
    return 2;
}

}  // namespace myteam
