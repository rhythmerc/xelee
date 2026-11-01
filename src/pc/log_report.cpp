/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "log_report.hpp"
#include "pc.h"
#include "input_poll.h"
#include "version.hpp"
#include "slippi/slp_user.h"

#include <SDL3/SDL.h>
#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <fstream>
#include <map>
#include <string>
#include <vector>

#if defined(__APPLE__)
#include <TargetConditionals.h>
#endif
#if defined(__ANDROID__)
#include <sys/system_properties.h>
#endif
#if !defined(_WIN32)
#include <sys/utsname.h>
#endif

/* The log drop's base URL (the XeleeNet service, tools/xeleenet/logs.go), compiled in from the
 * configure environment's XELEE_LOGS_URL (CMakeLists.txt; CI: a repository
 * secret). Without one Send logs saves the bundle instead; MELEE_LOGS_URL
 * overrides it at run time, e.g. for a local `xeleenet -logs DIR`. */
#ifndef PC_LOGS_URL_BUILTIN
#define PC_LOGS_URL_BUILTIN ""
#endif

namespace pc::log_report {
namespace {

constexpr size_t kFileCap = 1 << 20;   // the tail kept of each file
constexpr size_t kTotalCap = 4 << 20;  // the log drop takes at most 5 MB
constexpr int kMaxFiles = 6;           // three Quest sessions: the panel's log and the game's

std::string endpoint() {
    const char* env = std::getenv("MELEE_LOGS_URL");
    std::string url = env != nullptr && *env != '\0' ? env : PC_LOGS_URL_BUILTIN;
    while (!url.empty() && url.back() == '/')
        url.pop_back();
    return url;
}

const char* platform_name() {
#if defined(__ANDROID__) && defined(AURORA_ENABLE_OPENXR)
    return "quest";
#elif defined(__ANDROID__)
    return "android";
#elif defined(__APPLE__) && TARGET_OS_IPHONE
    return "ios";
#elif defined(__APPLE__)
    return "macos";
#elif defined(_WIN32)
    return "windows";
#else
    return "linux";
#endif
}

/* Which headset matters as much as which build: a Quest 2 and a Quest 3 are
 * different GPUs. */
std::string os_name() {
#if defined(__ANDROID__)
    char model[PROP_VALUE_MAX] = "", release[PROP_VALUE_MAX] = "", build[PROP_VALUE_MAX] = "";
    __system_property_get("ro.product.model", model);
    __system_property_get("ro.build.version.release", release);
    __system_property_get("ro.build.display.id", build);
    return std::string(model) + ", Android " + release + ", " + build;
#elif defined(_WIN32)
    return "Windows"; /* RUN-AND-LOG.bat's melee-pc-env.log has the version and the GPU driver */
#else
    struct utsname u;
    if (uname(&u) != 0)
        return SDL_GetPlatform();
    return std::string(u.sysname) + " " + u.release + " " + u.machine;
#endif
}

std::string stamp(const char* format) {
    const std::time_t now = std::time(nullptr);
    char out[32];
    std::strftime(out, sizeof out, format, std::gmtime(&now));
    return out;
}

/* At most the last `cap` bytes, from a line start. */
std::string tail(std::string text, size_t cap) {
    if (text.size() <= cap)
        return text;
    const size_t cut = text.size() - cap;
    const size_t nl = text.find('\n', cut);
    const size_t from = nl == std::string::npos ? cut : nl + 1;
    return "[... " + std::to_string(from) + " earlier bytes left out ...]\n" + text.substr(from);
}

bool read_file(const std::string& path, std::string& out) {
    std::ifstream f(path, std::ios::binary);
    if (!f)
        return false;
    f.seekg(0, std::ios::end);
    const std::streamoff size = f.tellg();
    if (size < 0)
        return false;
    /* Read only the tail; a frame log can run to hundreds of MB. It leaves
     * room for the "left out" line, so the cap in build() does not cut that. */
    const std::streamoff keep = std::streamoff(kFileCap - 64);
    const std::streamoff skip = size > keep ? size - keep : 0;
    f.seekg(skip);
    out.assign(size_t(size - skip), '\0');
    f.read(out.data(), std::streamsize(out.size()));
    out.resize(size_t(f.gcount()));
    if (skip > 0) {
        const size_t nl = out.find('\n');
        out = "[... " + std::to_string(skip + std::streamoff(nl == std::string::npos ? 0 : nl + 1)) +
              " earlier bytes left out ...]\n" + out.substr(nl == std::string::npos ? 0 : nl + 1);
    }
    return true;
}

void replace_all(std::string& s, const std::string& from, const std::string& to) {
    for (size_t at = s.find(from); at != std::string::npos; at = s.find(from, at + to.size()))
        s.replace(at, from.size(), to);
}

bool is_digit(char c) { return c >= '0' && c <= '9'; }

/* A dotted IPv4 address at `at`, ending at `end`; not one inside a longer
 * dotted number such as a version. */
bool parse_ipv4(const std::string& s, size_t at, uint32_t& ip, size_t& end) {
    uint32_t v = 0;
    size_t i = at;
    for (int part = 0; part < 4; ++part) {
        if (part > 0 && (i >= s.size() || s[i++] != '.'))
            return false;
        unsigned octet = 0, digits = 0;
        for (; i < s.size() && is_digit(s[i]) && digits < 4; ++i, ++digits)
            octet = octet * 10 + unsigned(s[i] - '0');
        if (digits == 0 || digits > 3 || octet > 255)
            return false;
        v = v << 8 | octet;
    }
    if (i < s.size() && (is_digit(s[i]) || (s[i] == '.' && i + 1 < s.size() && is_digit(s[i + 1]))))
        return false;
    ip = v;
    end = i;
    return true;
}

/* Loopback, LAN, link-local, multicast and broadcast addresses name no one,
 * and LAN play's logs need them. */
bool names_no_one(uint32_t ip) {
    const uint32_t a = ip >> 24, b = ip >> 16 & 0xff;
    return a == 0 || a == 10 || a == 127 || (a == 169 && b == 254) || (a == 172 && (b & 0xf0) == 16) ||
           (a == 192 && b == 168) || a >= 224;
}

/* A netplay log names every peer it dialed or heard from. Each other address
 * becomes ip-1, ip-2, ... in the order the bundle first shows it, with the
 * same number in every file, so the log still tells peers apart. */
void mask_ips(std::string& text, std::map<uint32_t, int>& seen) {
    std::string out;
    size_t copied = 0;
    for (size_t i = 0; i < text.size(); ++i) {
        if (!is_digit(text[i]) ||
            (i > 0 && (std::isalnum(static_cast<unsigned char>(text[i - 1])) || text[i - 1] == '.')))
            continue;
        uint32_t ip;
        size_t end;
        if (!parse_ipv4(text, i, ip, end))
            continue;
        if (!names_no_one(ip)) {
            const int n = seen.emplace(ip, int(seen.size()) + 1).first->second;
            out.append(text, copied, i - copied);
            out += "ip-" + std::to_string(n);
            copied = end;
        }
        i = end - 1;
    }
    if (copied == 0)
        return;
    out.append(text, copied, std::string::npos);
    text = std::move(out);
}

/* A desktop log names files under the player's home folder, whose name is
 * often theirs. */
void scrub(std::string& text, std::map<uint32_t, int>& ips) {
#if !defined(__ANDROID__)
    for (const char* var : {"HOME", "USERPROFILE"}) {
        const char* home = std::getenv(var);
        if (home == nullptr || std::strlen(home) <= 5)  // not "/" or "/root"
            continue;
        std::string h = home;
        replace_all(text, h, "~");
        std::replace(h.begin(), h.end(), '\\', '/');
        replace_all(text, h, "~");
    }
#endif
    mask_ips(text, ips);
}

bool exists(const std::string& path) { return SDL_GetPathInfo(path.c_str(), nullptr); }

/* The log of the session sending them first, then the newest others. */
std::vector<std::string> log_paths() {
    std::vector<std::string> out;
    auto add = [&](const std::string& path) {
        if (int(out.size()) < kMaxFiles && std::find(out.begin(), out.end(), path) == out.end() &&
            exists(path))
            out.push_back(path);
    };
    if (const char* current = pc_log_path())
        add(current);
#if defined(_WIN32)
    for (const char* name : {"melee-pc.log", "melee-pc.1.log", "melee-pc-env.log", "melee-frames.log"})
        add(name);
#else
    if (const char* dir = pc_log_dir()) {
        int n = 0;
        char** names = SDL_GlobDirectory(dir, "melee-*.log", 0, &n);
        std::vector<std::string> sorted;
        for (int i = 0; names != nullptr && i < n; ++i)
            sorted.emplace_back(names[i]);
        SDL_free(names);
        std::sort(sorted.rbegin(), sorted.rend());  // the names sort by time
        for (const auto& name : sorted)
            add(std::string(dir) + "/" + name);
    }
#endif
    return out;
}

#if defined(__ANDROID__)
/* An app reads its own processes' logcat lines, which hold what the log file
 * cannot: the Java side, the XR runtime's complaints about us, and a crash
 * from a build older than the crash handler's file lines. Cleared by a
 * reboot, so it covers the sessions since. */
std::string logcat() {
    std::string out;
    FILE* p = popen("logcat -d -v threadtime -t 10000 2>/dev/null", "r");
    if (p == nullptr)
        return out;
    char buf[1 << 14];
    for (size_t n; (n = std::fread(buf, 1, sizeof buf, p)) > 0;)
        out.append(buf, n);
    pclose(p);
    return tail(std::move(out), kFileCap);
}
#endif

std::string base_name(const std::string& path) {
    const size_t slash = path.find_last_of("/\\");
    return slash == std::string::npos ? path : path.substr(slash + 1);
}

std::string json_field(const std::string& body, const char* key) {
    const std::string quoted = std::string("\"") + key + "\"";
    size_t at = body.find(quoted);
    if (at == std::string::npos)
        return {};
    at = body.find('"', body.find(':', at + quoted.size()));
    if (at == std::string::npos)
        return {};
    const size_t end = body.find('"', at + 1);
    return end == std::string::npos ? std::string() : body.substr(at + 1, end - at - 1);
}

/* Where a player can reach the bundle without adb: the Quest's Files app
 * shows Download, and a desktop player already knows the log folder. */
std::string save(const Bundle& bundle) {
    const std::string name = "xelee-logs-" + stamp("%Y%m%d-%H%M%S") + ".txt";
    std::vector<std::string> paths;
#if defined(__ANDROID__)
    paths.push_back("/sdcard/Download/" + name);
#elif defined(_WIN32)
    paths.push_back(name);  // beside melee-pc.log
#endif
    if (const char* dir = pc_log_dir())
        paths.push_back(std::string(dir) + "/" + name);
    for (const auto& path : paths) {
        std::ofstream f(path, std::ios::binary);
        if (f && f.write(bundle.text.data(), std::streamsize(bundle.text.size())))
            return path;
    }
    return {};
}

}  // namespace

bool can_upload() { return !endpoint().empty(); }

/* One header, then each file as `===== FILE <name> <bytes> =====`, a newline
 * and exactly that many bytes; tools/fetch_logs.py splits on the counts, so a
 * log line that looks like a header cannot break it. */
Bundle build() {
    Bundle b;
    b.text = "XELEE-LOGS 1\n"
             "version: " + get_app_version() + "\n"
             "rev: " + std::string(pc_app_rev()) + "\n"
             "platform: " + platform_name() + "\n"
             "os: " + os_name() + "\n"
             "sent: " + stamp("%Y-%m-%dT%H:%M:%SZ") + "\n";
    size_t room = kTotalCap;
    std::map<uint32_t, int> ips;
    auto append = [&](const std::string& name, std::string body) {
        scrub(body, ips);
        body = tail(std::move(body), std::min(kFileCap, room));
        room -= std::min(room, body.size());
        b.text += "\n===== FILE " + name + " " + std::to_string(body.size()) + " =====\n" + body + "\n";
        b.files++;
    };
    for (const auto& path : log_paths()) {
        std::string body;
        if (room > 4096 && read_file(path, body))
            append(base_name(path), std::move(body));
    }
    {
        // Pads as SDL and the ports see them now; the game's log has them
        // each time one came or went ("pads:").
        std::string census(4096, '\0');
        census.resize(pc_pad_census(census.data(), census.size()));
        append("pads.txt", std::move(census));
    }
#if defined(__ANDROID__)
    if (room > 4096)
        append("logcat.txt", logcat());
#endif
    return b;
}

Result send(const Bundle& bundle) {
    Result r;
    const std::string url = endpoint();
    if (url.empty()) {
        r.error = "This build has no log server";
    } else {
        std::string reply(4096, '\0');
        int status = 0;
        const bool answered = pc_http_post((url + "/logs").c_str(), "text/plain; charset=utf-8",
            bundle.text.data(), bundle.text.size(), reply.data(), reply.size(), &status);
        reply.resize(std::strlen(reply.c_str()));
        if (answered && status == 200) {
            r.code = json_field(reply, "code");
            if (!r.code.empty()) {
                pc_log_line("logs: sent %zu bytes as %s", bundle.text.size(), r.code.c_str());
                return r;
            }
            r.error = "The log server's answer had no code";
        } else if (answered) {
            r.error = json_field(reply, "error");
            if (r.error.empty())
                r.error = "The log server answered HTTP " + std::to_string(status);
        } else {
            r.error = "Could not reach the log server";
        }
    }
    pc_log_line("logs: not sent (%s)", r.error.c_str());
    r.saved = save(bundle);
    return r;
}

}  // namespace pc::log_report
