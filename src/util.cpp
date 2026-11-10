#include "myteam/util.hpp"

#include <cctype>
#include <cstdlib>
#include <ctime>
#include <fstream>
#include <iterator>
#include <set>
#include <stdexcept>
#include <unistd.h>

namespace myteam {

std::string read_file(const fs::path& path) {
    auto content = try_read_file(path);
    if (!content) throw std::runtime_error("cannot read " + path.string());
    return std::move(*content);
}

std::optional<std::string> try_read_file(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return std::nullopt;
    return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

void write_file_atomic(const fs::path& path, std::string_view content) {
    if (path.has_parent_path()) fs::create_directories(path.parent_path());
    fs::path tmp = path;
    tmp += ".tmp-" + std::to_string(::getpid());
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        if (!out) throw std::runtime_error("cannot write " + tmp.string());
        out.write(content.data(), static_cast<std::streamsize>(content.size()));
        if (!out) throw std::runtime_error("cannot write " + tmp.string());
    }
    fs::rename(tmp, path);
}

void append_file(const fs::path& path, std::string_view content) {
    if (path.has_parent_path()) fs::create_directories(path.parent_path());
    std::ofstream out(path, std::ios::binary | std::ios::app);
    if (!out) throw std::runtime_error("cannot write " + path.string());
    out.write(content.data(), static_cast<std::streamsize>(content.size()));
}

std::string_view trim(std::string_view s) {
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.front()))) s.remove_prefix(1);
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back()))) s.remove_suffix(1);
    return s;
}

std::string lower(std::string_view s) {
    std::string out(s);
    for (char& c : out) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return out;
}

bool starts_with(std::string_view s, std::string_view prefix) { return s.substr(0, prefix.size()) == prefix; }

bool contains_ci(std::string_view haystack, std::string_view needle) {
    return lower(haystack).find(lower(needle)) != std::string::npos;
}

std::string join(const std::vector<std::string>& parts, std::string_view sep) {
    std::string out;
    for (std::size_t i = 0; i < parts.size(); ++i) {
        if (i) out += sep;
        out += parts[i];
    }
    return out;
}

std::vector<std::string> split_lines(std::string_view text) {
    std::vector<std::string> out;
    std::size_t start = 0;
    while (start < text.size()) {
        std::size_t end = text.find('\n', start);
        if (end == std::string_view::npos) end = text.size();
        std::string_view l = text.substr(start, end - start);
        if (!l.empty() && l.back() == '\r') l.remove_suffix(1);
        out.emplace_back(l);
        start = end + 1;
    }
    return out;
}

std::string normalize_handle(std::string_view handle) {
    handle = trim(handle);
    if (!handle.empty() && handle[0] == '@') handle.remove_prefix(1);
    return lower(handle);
}

std::string slug(std::string_view text, std::size_t max) {
    std::string out;
    for (char c : text) {
        unsigned char u = static_cast<unsigned char>(c);
        if (std::isalnum(u) && u < 128) {
            out += static_cast<char>(std::tolower(u));
        } else if (!out.empty() && out.back() != '-') {
            out += '-';
        }
        if (out.size() > max) {
            // Cut at the last word boundary rather than mid-word.
            std::size_t dash = out.rfind('-', max);
            out.resize(dash != std::string::npos && dash >= max / 2 ? dash : max);
            break;
        }
    }
    while (!out.empty() && out.back() == '-') out.pop_back();
    return out.empty() ? "task" : out;
}

std::string first_line(std::string_view text, std::size_t max) {
    text = trim(text);
    std::string out(trim(text.substr(0, text.find('\n'))));
    if (out.size() > max) {
        std::size_t cut = out.rfind(' ', max);
        out = out.substr(0, cut == std::string::npos || cut < max / 2 ? max : cut) + "...";
    }
    return out;
}

std::size_t estimate_tokens(std::string_view text) { return (text.size() + 3) / 4; }

static std::tm utc(Clock::time_point t) {
    std::time_t tt = Clock::to_time_t(t);
    std::tm tm{};
    gmtime_r(&tt, &tm);
    return tm;
}

std::string iso_timestamp(Clock::time_point t) {
    std::tm tm = utc(t);
    char buf[32];
    std::strftime(buf, sizeof buf, "%Y-%m-%dT%H:%M:%SZ", &tm);
    return buf;
}

std::string file_timestamp(Clock::time_point t) {
    std::tm tm = utc(t);
    char buf[32];
    std::strftime(buf, sizeof buf, "%Y%m%d-%H%M%S", &tm);
    return buf;
}

// Howard Hinnant's days_from_civil.
static long days_from_civil(long y, unsigned m, unsigned d) {
    y -= m <= 2;
    const long era = (y >= 0 ? y : y - 399) / 400;
    const unsigned yoe = static_cast<unsigned>(y - era * 400);
    const unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + static_cast<long>(doe) - 719468;
}

std::optional<long> parse_date_days(std::string_view date) {
    if (date.size() != 10 || date[4] != '-' || date[7] != '-') return std::nullopt;
    for (std::size_t i : {0u, 1u, 2u, 3u, 5u, 6u, 8u, 9u})
        if (!std::isdigit(static_cast<unsigned char>(date[i]))) return std::nullopt;
    long y = std::stol(std::string(date.substr(0, 4)));
    unsigned m = static_cast<unsigned>(std::stoul(std::string(date.substr(5, 2))));
    unsigned d = static_cast<unsigned>(std::stoul(std::string(date.substr(8, 2))));
    if (m < 1 || m > 12 || d < 1 || d > 31) return std::nullopt;
    return days_from_civil(y, m, d);
}

long days_since_epoch(Clock::time_point t) {
    return static_cast<long>(std::chrono::duration_cast<std::chrono::hours>(t.time_since_epoch()).count() / 24);
}

std::optional<fs::path> find_executable(std::string_view name) {
    if (name.find('/') != std::string_view::npos) {
        fs::path p(name);
        if (::access(p.c_str(), X_OK) == 0) return p;
        return std::nullopt;
    }
    const char* path = std::getenv("PATH");
    std::string dirs = path ? path : "/usr/local/bin:/usr/bin:/bin";
    // Agent CLIs are commonly installed per user.
    if (const char* home = std::getenv("HOME")) dirs += std::string(":") + home + "/.local/bin";
    std::size_t start = 0;
    while (start <= dirs.size()) {
        std::size_t end = dirs.find(':', start);
        if (end == std::string::npos) end = dirs.size();
        std::string dir = dirs.substr(start, end - start);
        if (!dir.empty()) {
            fs::path p = fs::path(dir) / std::string(name);
            if (::access(p.c_str(), X_OK) == 0) return p;
        }
        start = end + 1;
    }
    return std::nullopt;
}

namespace {

const std::set<std::string>& stop_words() {
    static const std::set<std::string> w = {
        "a",   "an",  "the", "to",   "of",   "in",   "on",  "for", "and", "or",   "is",   "are",
        "was", "be",  "it",  "i",    "we",   "my",   "our", "me",  "not", "no",   "does", "do",
        "did", "has", "have", "had", "that", "this", "with", "as", "at",  "by",   "from", "there",
        "so",  "too", "very", "can", "cannot", "could", "should", "would", "when", "what", "which"};
    return w;
}

std::set<std::string> words(std::string_view text) {
    std::set<std::string> out;
    std::string cur;
    auto flush = [&] {
        if (cur.size() > 1 && !stop_words().count(cur)) {
            // Crude stemming so "handoffs" and "handoff" match.
            if (cur.size() > 4 && cur.back() == 's') cur.pop_back();
            out.insert(cur);
        }
        cur.clear();
    };
    for (char c : lower(text)) {
        if (std::isalnum(static_cast<unsigned char>(c))) cur += c;
        else flush();
    }
    flush();
    return out;
}

}  // namespace

bool similar_text(std::string_view a, std::string_view b, double threshold) {
    auto wa = words(first_line(a, 300));
    auto wb = words(first_line(b, 300));
    if (wa.empty() || wb.empty()) return false;
    std::size_t common = 0;
    for (const auto& w : wa) common += wb.count(w);
    const std::size_t uni = wa.size() + wb.size() - common;
    // Jaccard on the content words; short titles need most of their words.
    return static_cast<double>(common) / static_cast<double>(uni) >= threshold;
}

bool same_subject(std::string_view a, std::string_view b) {
    if (similar_text(a, b)) return true;
    auto wa = words(first_line(a, 300));
    auto wb = words(first_line(b, 300));
    const std::size_t shorter = std::min(wa.size(), wb.size());
    if (shorter < 3) return false;
    std::size_t common = 0;
    for (const auto& w : wa) common += wb.count(w);
    // Most of the shorter one's words are in the longer one.
    return static_cast<double>(common) / static_cast<double>(shorter) >= 0.75;
}

std::optional<std::string> criterion_line(std::string_view line) {
    std::string l(trim(line));
    if (starts_with(l, "- ") || starts_with(l, "* ")) l = std::string(trim(std::string_view(l).substr(2)));
    if (starts_with(lower(l), "accept:")) l = std::string(trim(std::string_view(l).substr(7)));
    else if (starts_with(l, "( )")) l = std::string(trim(std::string_view(l).substr(3)));
    else return std::nullopt;
    if (l.empty()) return std::nullopt;
    return l;
}

std::string take_title_tags(const std::string& title, std::vector<std::string>& tags, std::optional<int>& priority) {
    std::string out;
    std::size_t i = 0;
    while (i < title.size()) {
        std::size_t j = title.find(' ', i);
        if (j == std::string::npos) j = title.size();
        std::string w = title.substr(i, j - i);
        std::string t = w;
        if (starts_with(t, "\\#")) t = t.substr(1);
        bool tag = t.size() > 1 && t[0] == '#' && std::isalpha(static_cast<unsigned char>(t[1]));
        for (std::size_t k = 1; tag && k < t.size(); ++k)
            tag = std::isalnum(static_cast<unsigned char>(t[k])) || t[k] == '-' || t[k] == '_';
        if (tag) {
            const std::string name = t.substr(1);
            if (name.size() == 2 && name[0] == 'p' && std::isdigit(static_cast<unsigned char>(name[1])))
                priority = name[1] - '0';
            else if (std::find(tags.begin(), tags.end(), name) == tags.end())
                tags.push_back(name);
        } else {
            if (!out.empty()) out += ' ';
            out += w;
        }
        i = j + 1;
    }
    return out;
}

std::pair<std::string, std::string> title_and_rest(std::string_view line, std::size_t max) {
    std::string l(trim(line));
    if (l.size() <= max) return {l, ""};
    std::size_t cut = std::string::npos;
    for (std::size_t i = 10; i + 1 < l.size() && i < max; ++i)
        if ((l[i] == '.' || l[i] == ':' || l[i] == ';') && l[i + 1] == ' ') {
            cut = i;
            break;
        }
    if (cut != std::string::npos) {
        std::string title = l.substr(0, cut);
        return {title, std::string(trim(std::string_view(l).substr(cut + 1)))};
    }
    std::size_t sp = l.rfind(' ', max);
    if (sp == std::string::npos || sp < max / 2) sp = max;
    return {l.substr(0, sp) + "...", std::string(trim(std::string_view(l).substr(sp)))};
}

std::string normalize_estimate(std::string_view in) {
    std::string e = lower(trim(in));
    if (!e.empty() && e[0] == '~') e.erase(0, 1);
    double hours = 0;
    bool any = false;
    std::size_t i = 0;
    while (i < e.size()) {
        while (i < e.size() && e[i] == ' ') ++i;
        std::size_t j = i;
        while (j < e.size() && (std::isdigit(static_cast<unsigned char>(e[j])) || e[j] == '.' || e[j] == ',')) ++j;
        if (j == i) break;
        std::string num = e.substr(i, j - i);
        for (char& c : num)
            if (c == ',') c = '.';
        double v = std::atof(num.c_str());
        while (j < e.size() && e[j] == ' ') ++j;
        std::size_t k = j;
        while (k < e.size() && std::isalpha(static_cast<unsigned char>(e[k]))) ++k;
        std::string unit = e.substr(j, k - j);
        if (unit == "d" || unit == "day" || unit == "days") {
            if (!any && k >= e.size() && v == static_cast<long>(v)) return std::to_string(static_cast<long>(v)) + "d";
            hours += v * 8;
        } else if (unit == "w" || unit == "wk" || unit == "week" || unit == "weeks") {
            if (!any && k >= e.size() && v == static_cast<long>(v)) return std::to_string(static_cast<long>(v)) + "w";
            hours += v * 40;
        } else if (unit == "m" || unit == "min" || unit == "mins" || unit == "minute" || unit == "minutes") {
            hours += v / 60;
        } else if (unit.empty() || unit == "h" || unit == "hr" || unit == "hrs" || unit == "hour" || unit == "hours") {
            hours += v;
        } else {
            return {};
        }
        any = true;
        i = k;
    }
    if (!any || hours <= 0) return {};
    long h = static_cast<long>(hours);
    if (hours > static_cast<double>(h) + 1e-9) ++h;
    return std::to_string(h) + "h";
}

std::string shell_quote(std::string_view s) {
    if (!s.empty() && s.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_-./=:@+,") ==
                          std::string_view::npos)
        return std::string(s);
    std::string out = "'";
    for (char c : s) {
        if (c == '\'') out += "'\\''";
        else out += c;
    }
    return out + "'";
}

}  // namespace myteam
