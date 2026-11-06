/*
 * Copyright 11/04/2026 https://github.com/su8/0verbs
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston,
 * MA 02110-1301, USA.
 */
#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <iostream>
#include <limits>
#include <map>
#include <optional>
#include <regex>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

#include <sys/stat.h>
#include <sys/utsname.h>
#include <unistd.h>

#include <bzlib.h>
#include <curl/curl.h>

namespace fs = std::filesystem;

// ---------------------------------------------------------------------------
// Configuration
// ---------------------------------------------------------------------------

static const char* PROGRAM_VERSION = "1.1.0";
static const char* INSTALLURL = "/etc/installurl";
static const char* OPENBSD_DEFAULT_MIRROR = "https://cdn.openbsd.org/pub/OpenBSD";
static const char* NETBSD_DEFAULT_MIRROR = "https://cdn.netbsd.org/pub/pkgsrc/packages/NetBSD";

static const std::vector<std::string> OPENBSD_FOLDERS = {"packages", "packages-stable"};

// The package directories on the OpenBSD and NetBSD mirrors do not always use
// the same architecture names as uname(3).  Normalise both the guessed and the
// user supplied --arch value through these tables.
static const std::map<std::string, std::string> OPENBSD_ARCH_NAMES = {
    {"x86_64", "amd64"}, {"amd64", "amd64"},
    {"arm64", "aarch64"}, {"aarch64", "aarch64"},
    {"i386", "i386"}, {"i486", "i386"}, {"i586", "i386"}, {"i686", "i386"}, {"x86", "i386"},
    {"armv7l", "arm"}, {"armv7", "arm"}, {"arm", "arm"},
    {"macppc", "powerpc"}, {"powerpc", "powerpc"},
    {"octeon", "mips64"}, {"loongson", "mips64"}, {"mips64", "mips64"}, {"mips64el", "mips64"},
    {"riscv64", "riscv64"},
    {"ppc64", "powerpc64"}, {"powerpc64", "powerpc64"},
    {"sparc64", "sparc64"},
};

static const std::map<std::string, std::string> NETBSD_ARCH_NAMES = {
    {"x86_64", "x86_64"}, {"amd64", "x86_64"},
    {"i386", "i386"}, {"i486", "i386"}, {"i586", "i386"}, {"i686", "i386"}, {"x86", "i386"},
    {"arm64", "aarch64"}, {"aarch64", "aarch64"},
    {"armv7l", "earmv7hf"}, {"armv7", "earmv7hf"}, {"armv7hf", "earmv7hf"}, {"earmv7hf", "earmv7hf"},
    {"armv6l", "earmv6hf"}, {"armv6", "earmv6hf"}, {"earmv6hf", "earmv6hf"},
    {"macppc", "macppc"},
    {"powerpc", "powerpc"},
    {"riscv64", "riscv64"},
    {"sparc64", "sparc64"},
    {"mips64", "mips64eb"}, {"mips64eb", "mips64eb"}, {"mips64el", "mipsel"},
    {"evbarm", "earmv7hf"},
};

static const std::regex PACKAGE_NAME(R"((.+?)-(\d[^-]*?)(?:p(\d+))?(?:v(\d+))?(?:nb(\d+))?((?:-[^-]+)*)$)");

static const std::string REGEX_SYNTAX = "^$*?()[]{}|\\";

static std::string g_cache_root;

// ---------------------------------------------------------------------------
// Small helpers
// ---------------------------------------------------------------------------

static std::string to_lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return s;
}

static std::string trim(const std::string& s) {
    size_t a = s.find_first_not_of(" \t\r\n");
    if (a == std::string::npos) return "";
    size_t b = s.find_last_not_of(" \t\r\n");
    return s.substr(a, b - a + 1);
}

static std::string strip_trailing_slashes(std::string s) {
    while (s.size() > 1 && (s.back() == '/' || s.back() == '\\')) s.pop_back();
    return s;
}

static std::string home_dir() {
    if (const char* h = std::getenv("HOME"); h && *h) return h;
    if (const char* h = std::getenv("USERPROFILE"); h && *h) return h;
    return ".";
}

static std::string expand_home(const std::string& p) {
    if (p == "~") return home_dir();
    if (p.size() >= 2 && p[0] == '~' && (p[1] == '/' || p[1] == '\\'))
        return (fs::path(home_dir()) / p.substr(2)).string();
    return p;
}

static std::string default_cache_root() {
    if (const char* x = std::getenv("XDG_CACHE_HOME"); x && *x)
        return (fs::path(x) / "0verbs").string();
    return (fs::path(home_dir()) / ".cache" / "0verbs").string();
}

static bool all_digits(const std::string& s) {
    return !s.empty() && std::all_of(s.begin(), s.end(), [](unsigned char c) {
        return std::isdigit(c) != 0;
    });
}

static void json_string(std::ostream& os, const std::string& s) {
    os << '"';
    for (unsigned char c : s) {
        switch (c) {
            case '"':  os << "\\\""; break;
            case '\\': os << "\\\\"; break;
            case '\b': os << "\\b";  break;
            case '\f': os << "\\f";  break;
            case '\n': os << "\\n";  break;
            case '\r': os << "\\r";  break;
            case '\t': os << "\\t";  break;
            default:
                if (c < 0x20) {
                    char buf[8];
                    std::snprintf(buf, sizeof(buf), "\\u%04x", static_cast<unsigned>(c));
                    os << buf;
                } else {
                    os << static_cast<char>(c);
                }
        }
    }
    os << '"';
}

// ---------------------------------------------------------------------------
// Version ordering
// ---------------------------------------------------------------------------

struct VersionOrder {
    std::vector<std::string> parts;

    bool operator<(const VersionOrder& o) const {
        size_t n = std::min(parts.size(), o.parts.size());
        for (size_t i = 0; i < n; ++i) {
            const std::string& a = parts[i];
            const std::string& b = o.parts[i];
            bool na = all_digits(a);
            bool nb = all_digits(b);
            if (na && nb) {
                long long ia = std::atoll(a.c_str());
                long long ib = std::atoll(b.c_str());
                if (ia != ib) return ia < ib;
            } else if (na != nb) {
                return na;  // numbers sort before non-numbers
            } else if (a != b) {
                return a < b;
            }
        }
        return parts.size() < o.parts.size();
    }
};

static VersionOrder version_order(const std::string& name) {
    VersionOrder v;
    std::string cur;
    for (char c : name) {
        if (std::isdigit(static_cast<unsigned char>(c))) {
            cur.push_back(c);
        } else {
            if (!cur.empty()) { v.parts.push_back(cur); cur.clear(); }
            v.parts.push_back(std::string(1, c));
        }
    }
    if (!cur.empty()) v.parts.push_back(cur);
    return v;
}

// Return the "line" of a version.  Most projects use the major version as a
// line (firefox 149/150), but for projects whose major version is below 10 the
// minor version is the line (php 8.3/8.4, python 3.12/3.13, ruby 3.3/3.4).
static std::string version_line(const std::string& version) {
    static const std::regex re(R"(^(\d+)(?:\.(\d+))?)");
    std::smatch m;
    if (!std::regex_search(version, m, re)) return "";
    long major = std::atol(m[1].str().c_str());
    if (major < 10 && m[2].matched) return m[1].str() + "." + m[2].str();
    return m[1].str();
}

// ---------------------------------------------------------------------------
// Package names
// ---------------------------------------------------------------------------

struct PackageParts {
    std::string stem;
    std::string version;
    std::string flavor;
    int p = -1;
    int v = -1;
    int nb = -1;
    bool ok = false;
};

static PackageParts parse_package(const std::string& name) {
    PackageParts pp;
    std::smatch m;
    if (std::regex_match(name, m, PACKAGE_NAME)) {
        try {
            pp.stem = m[1];
            pp.version = m[2];
            if (m[3].matched) pp.p = std::stoi(m[3]);
            if (m[4].matched) pp.v = std::stoi(m[4]);
            if (m[5].matched) pp.nb = std::stoi(m[5]);
            pp.flavor = m[6];
            pp.ok = true;
        } catch (const std::exception&) {
            pp = PackageParts{};  // treat absurd numeric suffixes as unparseable
        }
    }
    return pp;
}

// Used to decide whether an installed package corresponds to a package in the
// list.  Versions are deliberately ignored: firefox-149 installed and
// firefox-150 available are the same package line.
static std::string stem_flavor_key(const std::string& name) {
    PackageParts p = parse_package(name);
    if (!p.ok) return name;
    return p.stem + "\x01" + p.flavor;
}

// ---------------------------------------------------------------------------
// Platform / architecture
// ---------------------------------------------------------------------------

static std::string machine_arch() {
    struct utsname u;
    if (uname(&u) != 0) return "";
    return to_lower(u.machine);
}

static std::string normalize_arch(const std::map<std::string, std::string>& names,
                                  const std::string& arch) {
    std::string a = to_lower(trim(arch));
    auto it = names.find(a);
    return it != names.end() ? it->second : a;
}

static std::string openbsd_default_arch() {
    return normalize_arch(OPENBSD_ARCH_NAMES, machine_arch());
}

static std::string netbsd_default_arch() {
    return normalize_arch(NETBSD_ARCH_NAMES, machine_arch());
}

static std::string host_sysname() {
    struct utsname u;
    if (uname(&u) != 0) return "";
    return u.sysname;
}

static std::string host_release() {
    struct utsname u;
    if (uname(&u) != 0) return "";
    return u.release;
}

// "10.1" -> "10.", the prefix used to match a NetBSD release directory.
static std::string host_major_prefix() {
    std::string release = host_release();
    if (release.empty()) return "";
    return release.substr(0, release.find('.')) + ".";
}

static std::string first_line(const std::string& s) {
    size_t nl = s.find('\n');
    return nl == std::string::npos ? s : s.substr(0, nl);
}

static std::string sysctl_kern_version() {
    std::string out;
    FILE* fp = popen("sysctl -n kern.version 2>/dev/null", "r");
    if (!fp) return out;
    char buf[256];
    while (std::fgets(buf, static_cast<int>(sizeof(buf)), fp) != nullptr) out += buf;
    (void)pclose(fp);
    return out;
}

static std::string detect_os() {
    return host_sysname() == "OpenBSD" ? "openbsd" : "netbsd";
}

// ---------------------------------------------------------------------------
// HTTP
// ---------------------------------------------------------------------------

struct HttpResult {
    bool ok = false;
    bool missing = false;
    long status = 0;
    std::string body;
    std::string error;
};

struct CurlWrite {
    std::string* out;
};

static size_t curl_write_cb(void* ptr, size_t size, size_t nmemb, void* userdata) {
    auto* w = static_cast<CurlWrite*>(userdata);
    w->out->append(static_cast<char*>(ptr), size * nmemb);
    return size * nmemb;
}

static HttpResult http_get(const std::string& url, long timeout, bool missing_ok) {
    HttpResult result;

    CURL* curl = curl_easy_init();
    if (!curl) {
        result.error = "could not initialise libcurl";
        return result;
    }

    const std::string user_agent = "0verbs/" + std::string(PROGRAM_VERSION);
    CurlWrite writer{&result.body};
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, curl_write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &writer);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, timeout);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, std::min<long>(timeout, 20));
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_MAXREDIRS, 10L);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, user_agent.c_str());
#if LIBCURL_VERSION_NUM >= 0x075500
    curl_easy_setopt(curl, CURLOPT_PROTOCOLS_STR, "http,https");
    curl_easy_setopt(curl, CURLOPT_REDIR_PROTOCOLS_STR, "http,https");
#else
    curl_easy_setopt(curl, CURLOPT_PROTOCOLS, static_cast<long>(CURLPROTO_HTTP | CURLPROTO_HTTPS));
    curl_easy_setopt(curl, CURLOPT_REDIR_PROTOCOLS, static_cast<long>(CURLPROTO_HTTP | CURLPROTO_HTTPS));
#endif

    // One retry for transient problems and 5xx responses.
    for (int attempt = 0; attempt < 2; ++attempt) {
        result.body.clear();
        CURLcode res = curl_easy_perform(curl);
        long code = 0;
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &code);
        result.status = code;

        if (res == CURLE_OK) {
            if (code >= 200 && code < 300) {
                result.ok = true;
                break;
            }
            if (code == 404 && missing_ok) {
                result.ok = true;
                result.missing = true;
                result.body.clear();
                break;
            }
            if (code == 404) {
                result.error = "HTTP 404 for " + url +
                    "\nUse --release and/or --arch to pick a release and architecture that exist on the mirror";
                break;
            }
            if (code >= 500 && attempt == 0) continue;
            result.error = "HTTP " + std::to_string(code) + " for " + url;
            break;
        }

        bool transient =
            res == CURLE_OPERATION_TIMEDOUT ||
            res == CURLE_COULDNT_CONNECT ||
            res == CURLE_COULDNT_RESOLVE_HOST ||
            res == CURLE_GOT_NOTHING ||
            res == CURLE_PARTIAL_FILE;
        if (transient && attempt == 0) continue;

        result.error = "could not download " + url + ": " + curl_easy_strerror(res);
        break;
    }

    curl_easy_cleanup(curl);
    return result;
}

static bool fetch(const std::string& url, std::string& out, long timeout,
                  bool announce, bool missing_ok, std::string& error) {
    if (announce) std::cerr << "Downloading " << url << " ... " << std::flush;
    HttpResult r = http_get(url, timeout, missing_ok);
    if (announce) std::cerr << "\n";
    out = std::move(r.body);
    if (!r.ok) {
        error = r.error;
        return false;
    }
    return true;
}

// ---------------------------------------------------------------------------
// Cache
// ---------------------------------------------------------------------------

static std::string local_list(const std::string& flavour, const std::string& release,
                              const std::string& arch, const std::string& folder = "packages",
                              const std::string& filename = "SHA256") {
    return (fs::path(g_cache_root) / flavour / release / folder / arch / filename).string();
}

static bool file_exists(const std::string& path) {
    std::error_code ec;
    return fs::is_regular_file(path, ec);
}

static bool atomic_write(const std::string& path, const std::string& data, std::string& error) {
    std::error_code ec;
    fs::path p(path);

    if (p.has_parent_path()) {
        fs::create_directories(p.parent_path(), ec);
        if (ec) {
            error = "could not create " + p.parent_path().string() + ": " + ec.message();
            return false;
        }
    }

    fs::path tmp = p;
    tmp += ".tmp." + std::to_string(static_cast<long>(getpid()));

    {
        std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
        if (!f) {
            error = "could not open " + tmp.string();
            return false;
        }
        f.write(data.data(), static_cast<std::streamsize>(data.size()));
        f.flush();
        if (!f) {
            error = "could not write " + tmp.string();
            f.close();
            fs::remove(tmp, ec);
            return false;
        }
    }

    fs::rename(tmp, p, ec);
#ifdef _WIN32
    // Windows does not replace an existing destination with rename().
    if (ec) {
        std::error_code ignored;
        fs::remove(p, ignored);
        ec.clear();
        fs::rename(tmp, p, ec);
    }
#endif
    if (ec) {
        std::error_code ignored;
        fs::remove(tmp, ignored);
        error = "could not replace " + p.string() + ": " + ec.message();
        return false;
    }
    return true;
}

static std::vector<std::string> cached_releases(const std::string& os, const std::string& arch) {
    std::vector<std::string> out;
    fs::path root = fs::path(g_cache_root) / os;
    std::error_code ec;
    if (!fs::is_directory(root, ec)) return out;

    for (fs::directory_iterator it(root, ec), end; !ec && it != end; it.increment(ec)) {
        std::error_code e2;
        if (!it->is_directory(e2)) continue;
        fs::path p = it->path() / "packages" / arch;
        if (file_exists((p / "SHA256").string()) || file_exists((p / "SHA512").string()))
            out.push_back(it->path().filename().string());
    }

    std::sort(out.begin(), out.end(), [](const std::string& a, const std::string& b) {
        return version_order(a) < version_order(b);
    });
    return out;
}

static std::string latest_cached_release(const std::string& os, const std::string& arch) {
    std::vector<std::string> releases = cached_releases(os, arch);
    // "snapshots" sorts after every number, but it is only wanted when asked
    // for with --release or when it is all that is cached.
    for (auto it = releases.rbegin(); it != releases.rend(); ++it)
        if (*it != "snapshots") return *it;
    return releases.empty() ? "" : releases.back();
}

// Newest cached release whose name starts with a major-version prefix such
// as "10.".  NetBSD packages only install on the same major release.
static std::string latest_cached_major(const std::string& os, const std::string& arch,
                                       const std::string& prefix) {
    std::vector<std::string> releases = cached_releases(os, arch);
    for (auto it = releases.rbegin(); it != releases.rend(); ++it)
        if (it->rfind(prefix, 0) == 0) return *it;
    return "";
}

// ---------------------------------------------------------------------------
// Mirrors
// ---------------------------------------------------------------------------

static bool valid_mirror(const std::string& url) {
    return url.rfind("https://", 0) == 0 || url.rfind("http://", 0) == 0;
}

static std::string openbsd_mirror(const std::string& override_url) {
    if (!override_url.empty()) return override_url;

    std::ifstream f(INSTALLURL);
    std::string url;
    if (f && std::getline(f, url)) {
        url = strip_trailing_slashes(trim(url));
        if (valid_mirror(url)) return url;
        if (!url.empty())
            std::cerr << "Warning: ignoring invalid mirror in " << INSTALLURL << "\n";
    }
    return OPENBSD_DEFAULT_MIRROR;
}

static std::string netbsd_mirror(const std::string& override_url) {
    return override_url.empty() ? std::string(NETBSD_DEFAULT_MIRROR) : override_url;
}

// ---------------------------------------------------------------------------
// Release discovery
// ---------------------------------------------------------------------------

static bool openbsd_mirror_releases(const std::string& mirror, long timeout,
                                    std::vector<std::string>& out, std::string& error) {
    out.clear();
    std::string listing;
    if (!fetch(mirror + "/", listing, timeout, false, false, error)) return false;

    std::regex re(R"(href="(?:[^"]*/)?(\d+\.\d+)/")");
    for (auto it = std::sregex_iterator(listing.begin(), listing.end(), re);
         it != std::sregex_iterator(); ++it) {
        out.push_back((*it)[1]);
    }
    std::sort(out.begin(), out.end(), [](const std::string& a, const std::string& b) {
        return version_order(a) < version_order(b);
    });
    out.erase(std::unique(out.begin(), out.end()), out.end());
    if (out.empty()) {
        error = "could not find any releases at " + mirror + "/";
        return false;
    }
    return true;
}

static bool openbsd_default_release(const std::string& mirror, long timeout, std::string& out) {
    if (host_sysname() == "OpenBSD") {
        // Like pkg_add(1): -current and -beta kernels use snapshots.  On a
        // regular machine use the installed release, without asking the
        // mirror first.
        static const std::regex current_beta(R"(-(current|beta)\b)");
        if (std::regex_search(first_line(sysctl_kern_version()), current_beta)) {
            out = "snapshots";
            return true;
        }
        std::string release = host_release();
        if (!release.empty()) {
            out = release;
            return true;
        }
    }

    // Other systems (or a machine where uname(3) had no release) use the
    // newest release on the mirror.
    std::vector<std::string> releases;
    std::string error;
    if (!openbsd_mirror_releases(mirror, timeout, releases, error)) {
        std::cerr << "ERROR: " << error << "\n";
        return false;
    }
    out = releases.back();
    return true;
}

static bool preview_release(const std::string& release) {
    std::string r = to_lower(release);
    return r.find("rc") != std::string::npos ||
           r.find("beta") != std::string::npos ||
           r.find("alpha") != std::string::npos;
}

static bool netbsd_mirror_releases(const std::string& mirror, const std::string& arch,
                                   long timeout, std::vector<std::string>& out,
                                   std::string& error) {
    out.clear();
    std::string listing;
    if (!fetch(mirror + "/" + arch + "/", listing, timeout, false, false, error)) return false;

    // Handles both 10.0 and 10.0_2026Q3 (the Q is uppercase).
    std::regex re(R"(href="(?:[^"]*/)?(\d+\.\d+(?:_[A-Za-z0-9]+)?)/")");
    for (auto it = std::sregex_iterator(listing.begin(), listing.end(), re);
         it != std::sregex_iterator(); ++it) {
        out.push_back((*it)[1]);
    }
    std::sort(out.begin(), out.end(), [](const std::string& a, const std::string& b) {
        return version_order(a) < version_order(b);
    });
    out.erase(std::unique(out.begin(), out.end()), out.end());
    if (out.empty()) {
        error = "could not find any releases for arch " + arch + " at " + mirror + "/" + arch + "/";
        return false;
    }
    return true;
}

static bool netbsd_default_release(const std::string& mirror, const std::string& arch,
                                   long timeout, std::string& out) {
    std::vector<std::string> releases;
    std::string error;
    if (!netbsd_mirror_releases(mirror, arch, timeout, releases, error)) {
        std::cerr << "ERROR: " << error << "\n";
        return false;
    }

    // Packages only install on the same major release (10.x, 9.x).  Prefer
    // the newest release of the host's major version when running on NetBSD.
    if (host_sysname() == "NetBSD") {
        std::string major = host_major_prefix();
        std::string same;
        if (!major.empty()) {
            for (const auto& r : releases) {
                if (r.rfind(major, 0) == 0) same = r;
            }
        }
        if (!same.empty()) {
            out = same;
            return true;
        }
    }

    // Otherwise prefer the newest non-preview release.  If only previews
    // exist, use the newest one.
    std::string chosen;
    for (const auto& r : releases) {
        if (!preview_release(r)) chosen = r;
    }
    if (chosen.empty()) chosen = releases.back();
    out = chosen;
    return true;
}

// ---------------------------------------------------------------------------
// OpenBSD backend
// ---------------------------------------------------------------------------

static bool openbsd_download(const std::string& mirror, const std::string& release,
                             const std::string& arch, const std::string& folder,
                             long timeout) {
    std::string url = mirror + "/" + release + "/" + folder + "/" + arch + "/SHA256";
    std::string db = local_list("openbsd", release, arch, folder);
    std::string data;
    std::string error;
    const bool missing_ok = (folder != "packages");

    if (!fetch(url, data, timeout, true, missing_ok, error)) {
        std::cerr << "ERROR: " << error << "\n";
        return false;
    }
    if (data.empty() && folder == "packages") {
        std::cerr << "ERROR: empty listing for " << url << "\n";
        return false;
    }

    if (!atomic_write(db, data, error)) {
        std::cerr << "ERROR: " << error << "\n";
        return false;
    }

    if (data.empty()) std::cerr << "no updates yet\n";
    else std::cerr << "saved to " << db << "\n";
    return true;
}

static std::vector<std::pair<std::string, std::string>>
openbsd_parse(const std::string& db_path) {
    std::vector<std::pair<std::string, std::string>> out;
    std::ifstream f(db_path);
    if (!f) return out;

    // OpenBSD's SHA256 file contains base64 digests, not hex.  \S+ is used so
    // that +, / and = are accepted too.
    std::regex re(R"(SHA256 \((?:All/)?(.+?)\.tgz\) = (\S+))");
    std::string line;
    while (std::getline(f, line)) {
        std::smatch m;
        if (std::regex_search(line, m, re)) {
            std::string name = m[1];
            if (name.rfind("All/", 0) == 0) name = name.substr(4);
            out.emplace_back(name, m[2]);
        }
    }
    return out;
}

// ---------------------------------------------------------------------------
// NetBSD backend
// ---------------------------------------------------------------------------

static bool decompress_bz2(const std::string& in, std::string& out, std::string& error) {
    if (in.size() > std::numeric_limits<unsigned int>::max()) {
        error = "bzip2 input is too large";
        return false;
    }

    bz_stream strm{};
    int rc = BZ2_bzDecompressInit(&strm, 0, 0);
    if (rc != BZ_OK) {
        error = "could not initialise bzip2";
        return false;
    }

    out.clear();
    char buf[16384];
    strm.next_in = const_cast<char*>(in.data());
    strm.avail_in = static_cast<unsigned>(in.size());

    for (;;) {
        strm.next_out = buf;
        strm.avail_out = sizeof(buf);
        int ret = BZ2_bzDecompress(&strm);
        if (ret != BZ_OK && ret != BZ_STREAM_END) {
            BZ2_bzDecompressEnd(&strm);
            error = "invalid bzip2 data";
            return false;
        }
        out.append(buf, sizeof(buf) - strm.avail_out);
        if (ret == BZ_STREAM_END) break;
        if (strm.avail_in == 0 && strm.avail_out == sizeof(buf)) {
            BZ2_bzDecompressEnd(&strm);
            error = "truncated bzip2 data";
            return false;
        }
    }

    BZ2_bzDecompressEnd(&strm);
    return true;
}

static bool netbsd_download(const std::string& mirror, const std::string& release,
                            const std::string& arch, long timeout) {
    std::string url = mirror + "/" + arch + "/" + release + "/SHA512.bz2";
    std::string db = local_list("netbsd", release, arch, "packages", "SHA512");
    std::string data;
    std::string error;

    if (!fetch(url, data, timeout, true, false, error)) {
        std::cerr << "ERROR: " << error << "\n";
        return false;
    }

    std::string text;
    if (!decompress_bz2(data, text, error)) {
        std::cerr << "ERROR: " << error << "\n";
        return false;
    }

    // Keep only the decompressed list, as the Python version does.
    if (!atomic_write(db, text, error)) {
        std::cerr << "ERROR: " << error << "\n";
        return false;
    }

    std::cerr << "saved to " << db << "\n";
    return true;
}

static std::vector<std::pair<std::string, std::string>>
netbsd_parse(const std::string& db_path) {
    std::vector<std::pair<std::string, std::string>> out;
    std::ifstream f(db_path);
    if (!f) return out;

    std::regex re(R"(SHA512 \((?:All/)?(.+?)\.tgz\) = (\S+))");
    std::string line;
    while (std::getline(f, line)) {
        std::smatch m;
        if (std::regex_search(line, m, re))
            out.emplace_back(m[1], m[2]);
    }
    return out;
}

// ---------------------------------------------------------------------------
// Search helpers
// ---------------------------------------------------------------------------

static bool compile_word(const std::string& word, bool fixed, std::regex& out,
                         std::string& error) {
    bool has_syntax = false;
    if (!fixed) {
        for (unsigned char c : word) {
            if (REGEX_SYNTAX.find(static_cast<char>(c)) != std::string::npos) {
                has_syntax = true;
                break;
            }
        }
    }

    std::string pattern;
    if (!has_syntax) {
        static const std::string special = R"(\.^$|()[]{}*+?)";
        for (char c : word) {
            if (special.find(c) != std::string::npos) pattern.push_back('\\');
            pattern.push_back(c);
        }
    } else {
        pattern = word;
    }

    try {
        out = std::regex(pattern, std::regex::ECMAScript | std::regex::icase);
    } catch (const std::regex_error& e) {
        error = "invalid regular expression '" + word + "': " + e.what();
        return false;
    }
    return true;
}

static bool exact_name(const std::string& name, const std::vector<std::regex>& patterns) {
    PackageParts parts = parse_package(name);
    for (const auto& pat : patterns) {
        if (std::regex_match(name, pat)) return true;
        if (parts.ok && std::regex_match(parts.stem, pat)) return true;
    }
    return false;
}

struct Match {
    std::string name;
    std::string label;
    std::string sha;
    std::string url;
};

struct Source {
    std::string label;       // "", "stable", ...
    std::string base_url;    // directory that contains the .tgz files
    std::vector<std::pair<std::string, std::string>> items;
};

struct InstalledPackages {
    std::set<std::string> names;
    std::set<std::string> keys;
};

static InstalledPackages read_installed(const std::string& os) {
    InstalledPackages out;
    std::vector<std::string> dirs;
    if (os == "openbsd") dirs = {"/var/db/pkg"};
    else dirs = {"/usr/pkg/pkgdb", "/var/db/pkg"};

    for (const auto& dir : dirs) {
        std::error_code ec;
        if (!fs::is_directory(dir, ec)) continue;

        fs::directory_iterator it(dir, ec), end;
        while (!ec && it != end) {
            std::error_code e2;
            if (it->is_directory(e2)) {
                std::string name = it->path().filename().string();
                if (!name.empty() && name[0] != '.') {
                    out.names.insert(name);
                    out.keys.insert(stem_flavor_key(name));
                }
            }
            it.increment(ec);
        }

        if (!out.names.empty()) break;
    }
    return out;
}

static bool is_installed(const Match& m, const InstalledPackages* installed) {
    if (!installed) return false;
    if (installed->names.count(m.name)) return true;
    return installed->keys.count(stem_flavor_key(m.name)) != 0;
}

// Rank is (stem branch, epoch, numeric-aware version, pkgfix, pkgsrc
// revision).
static std::vector<Match> keep_newest(const std::vector<Match>& matches,
                                      bool stem_only = false) {
    using Rank = std::tuple<long long, int, VersionOrder, int, int>;

    struct Key {
        std::string stem;
        std::string flavor;
        std::string line;

        bool operator<(const Key& o) const {
            if (stem != o.stem) return stem < o.stem;
            if (flavor != o.flavor) return flavor < o.flavor;
            return line < o.line;
        }
    };

    std::map<Key, std::pair<Rank, Match>> newest;

    for (const auto& match : matches) {
        PackageParts parts = parse_package(match.name);

        if (!parts.ok) {
            Key k{match.name, "", ""};
            if (!newest.count(k))
                newest[k] = {Rank{0, 0, VersionOrder{}, -1, -1}, match};
            continue;
        }

        long long branch = 0;
        Key k;
        if (stem_only) {
            // NetBSD: collapse by stem only (jdk-17 and jdk-21 become one
            // row).  pkgsrc also puts the branch in a stem prefix
            // (php84-curl, py313-yaml), so collapse that too; the highest
            // branch wins.  wget2 has no "-" after the digits and stays as
            // it is.
            std::string stem = parts.stem;
            static const std::regex prefixed(R"(([A-Za-z]+)(\d+)(-.+))");
            std::smatch m;
            if (std::regex_match(stem, m, prefixed)) {
                try {
                    branch = std::stoll(m[2].str());
                } catch (const std::exception&) {
                    branch = 0;
                }
                stem = m[1].str() + m[3].str();
            }
            k = {stem, "", ""};
        } else {
            // OpenBSD: keep php-8.3 and php-8.4 separate, but collapse
            // php-8.4.1 and php-8.4.2.
            k = {parts.stem, parts.flavor, version_line(parts.version)};
        }

        // Python treats a missing epoch as zero; p and nb keep -1 so that
        // p0/nb0 sort above an absent suffix.
        int epoch = parts.v < 0 ? 0 : parts.v;
        Rank rank{branch, epoch, version_order(parts.version), parts.p, parts.nb};
        auto it = newest.find(k);
        if (it == newest.end() || rank > it->second.first)
            newest[k] = {rank, match};
    }

    std::vector<Match> out;
    out.reserve(newest.size());
    for (auto& kv : newest) out.push_back(kv.second.second);
    return out;
}

static std::vector<Match> hide_superseded(const std::vector<Match>& matches) {
    auto line_key = [](const std::string& name) -> std::optional<std::string> {
        PackageParts parts = parse_package(name);
        if (!parts.ok) return std::nullopt;
        return parts.stem + "\x01" + parts.flavor + "\x01" + version_line(parts.version);
    };

    auto package_key = [](const std::string& name) -> std::optional<std::string> {
        PackageParts parts = parse_package(name);
        if (!parts.ok) return std::nullopt;
        return parts.stem + "\x01" + parts.flavor;
    };

    // Most packages have one version per release, and any stable update
    // replaces it (curl-8.16.0 -> curl-8.19.0, a different version_line).
    // Only packages with several branches in the release (php-8.3, php-8.4)
    // are matched per version_line.
    std::set<std::string> updated, updated_package;
    std::map<std::string, std::set<std::string>> release_lines;
    for (const auto& m : matches) {
        auto k = line_key(m.name);
        if (!k) continue;
        if (!m.label.empty()) {
            updated.insert(*k);
            updated_package.insert(*package_key(m.name));
        } else {
            release_lines[*package_key(m.name)].insert(*k);
        }
    }

    std::vector<Match> out;
    for (const auto& m : matches) {
        if (!m.label.empty()) {
            out.push_back(m);
            continue;
        }
        auto k = line_key(m.name);
        if (!k) {
            out.push_back(m);
            continue;
        }
        std::string pk = *package_key(m.name);
        bool superseded = release_lines[pk].size() > 1 ? updated.count(*k) != 0
                                                       : updated_package.count(pk) != 0;
        if (!superseded) out.push_back(m);
    }
    return out;
}

static bool match_less(const Match& a, const Match& b) {
    PackageParts pa = parse_package(a.name);
    PackageParts pb = parse_package(b.name);

    if (pa.ok && pb.ok) {
        if (pa.stem != pb.stem) return pa.stem < pb.stem;
        VersionOrder va = version_order(pa.version);
        VersionOrder vb = version_order(pb.version);
        if (va < vb) return true;
        if (vb < va) return false;
        if (pa.flavor != pb.flavor) return pa.flavor < pb.flavor;
        if (pa.p != pb.p) return pa.p < pb.p;
        if (pa.v != pb.v) return pa.v < pb.v;
        if (pa.nb != pb.nb) return pa.nb < pb.nb;
    } else if (pa.ok != pb.ok) {
        return pa.ok;  // parseable names first
    }

    if (a.label != b.label) return a.label < b.label;
    return a.name < b.name;
}

static size_t edit_distance(const std::string& a, const std::string& b) {
    if (a.size() > b.size()) return edit_distance(b, a);

    std::vector<size_t> prev(b.size() + 1), cur(b.size() + 1);
    for (size_t j = 0; j <= b.size(); ++j) prev[j] = j;

    for (size_t i = 1; i <= a.size(); ++i) {
        cur[0] = i;
        for (size_t j = 1; j <= b.size(); ++j) {
            size_t cost = (a[i - 1] == b[j - 1]) ? 0 : 1;
            cur[j] = std::min({prev[j] + 1, cur[j - 1] + 1, prev[j - 1] + cost});
        }
        prev.swap(cur);
    }
    return prev[b.size()];
}

static std::vector<std::string> suggest(const std::vector<std::string>& pool,
                                        const std::string& query, size_t max_results) {
    std::string q = to_lower(query);
    if (q.empty()) return {};

    std::set<std::string> stems;
    for (const auto& name : pool) {
        PackageParts p = parse_package(name);
        stems.insert(p.ok ? p.stem : name);
    }

    struct Cand {
        std::string name;
        double score;
    };
    std::vector<Cand> cands;

    for (const auto& stem : stems) {
        if (stem.size() > 64 || q.size() > 64) continue;
        std::string s = to_lower(stem);
        size_t dist = edit_distance(q, s);
        double sim = 1.0 - static_cast<double>(dist) /
                            static_cast<double>(std::max(q.size(), s.size()));
        if (s.rfind(q, 0) == 0) sim = std::max(sim, 0.85);
        if (s.find(q) != std::string::npos) sim = std::max(sim, 0.75);
        if (sim >= 0.55) cands.push_back({stem, sim});
    }

    std::sort(cands.begin(), cands.end(), [](const Cand& a, const Cand& b) {
        if (a.score != b.score) return a.score > b.score;
        if (a.name.size() != b.name.size()) return a.name.size() < b.name.size();
        return a.name < b.name;
    });

    std::vector<std::string> out;
    for (size_t i = 0; i < cands.size() && out.size() < max_results; ++i)
        out.push_back(cands[i].name);
    return out;
}

// ---------------------------------------------------------------------------
// Output / search
// ---------------------------------------------------------------------------

struct SearchOptions {
    bool show_hash = false;
    bool exact = false;
    bool json = false;
    bool show_url = false;
    bool annotate_installed = false;
    bool installed_only = false;
    bool no_suggest = false;
    bool collapse_newest = false;
    bool stem_only = false;
    long limit = -1;
    long max_age_days = 7;
    std::string hash_label;
    std::string os;
    std::string release;
    std::string arch;
    const InstalledPackages* installed = nullptr;
};

static void print_json(const std::vector<Match>& matches, const SearchOptions& so,
                       const std::vector<std::string>& suggestions) {
    std::cout << "{";
    std::cout << "\"os\":";       json_string(std::cout, so.os);
    std::cout << ",\"release\":"; json_string(std::cout, so.release);
    std::cout << ",\"arch\":";    json_string(std::cout, so.arch);
    std::cout << ",\"count\":" << matches.size();
    std::cout << ",\"packages\":[";

    bool first = true;
    for (const auto& m : matches) {
        if (!first) std::cout << ",";
        first = false;

        PackageParts pp = parse_package(m.name);
        std::cout << "{";
        std::cout << "\"name\":"; json_string(std::cout, m.name);
        std::cout << ",\"version\":";
        if (pp.ok) json_string(std::cout, pp.version);
        else std::cout << "null";
        std::cout << ",\"flavor\":";
        if (pp.ok && !pp.flavor.empty()) json_string(std::cout, pp.flavor);
        else std::cout << "null";
        std::cout << ",\"hash_type\":"; json_string(std::cout, to_lower(so.hash_label));
        std::cout << ",\"hash\":";      json_string(std::cout, m.sha);
        std::cout << ",\"url\":";       json_string(std::cout, m.url);
        std::cout << ",\"branch\":";
        if (m.label.empty()) std::cout << "null";
        else json_string(std::cout, m.label);
        std::cout << ",\"installed\":" << (is_installed(m, so.installed) ? "true" : "false");
        std::cout << "}";
    }
    std::cout << "]";

    if (!suggestions.empty()) {
        std::cout << ",\"suggestions\":[";
        for (size_t i = 0; i < suggestions.size(); ++i) {
            if (i) std::cout << ",";
            json_string(std::cout, suggestions[i]);
        }
        std::cout << "]";
    }
    std::cout << "}\n";
}

static void print_text(const std::vector<Match>& matches, const SearchOptions& so,
                       const std::vector<std::string>& suggestions) {
    if (!suggestions.empty()) {
        std::cout << "\nDid you mean?\n";
        for (const auto& s : suggestions) std::cout << "  " << s << "\n";
        return;
    }

    for (const auto& m : matches) {
        std::cout << m.name;
        if (!m.label.empty()) std::cout << " (" << m.label << ")";
        if (so.annotate_installed && is_installed(m, so.installed))
            std::cout << " [installed]";
        std::cout << "\n";
        if (so.show_hash) std::cout << so.hash_label << ": " << m.sha << "\n";
        if (so.show_url) std::cout << "URL: " << m.url << "\n";
    }

    if (matches.empty()) std::cerr << "No matches found.\n";
}

static void print_releases(const std::vector<std::string>& releases, bool json) {
    if (json) {
        std::cout << "[";
        for (size_t i = 0; i < releases.size(); ++i) {
            if (i) std::cout << ",";
            json_string(std::cout, releases[i]);
        }
        std::cout << "]\n";
        return;
    }
    for (const auto& r : releases) std::cout << r << "\n";
}

static void warn_if_stale(const std::vector<std::string>& paths, long max_age_days) {
    if (max_age_days <= 0) return;

    time_t newest = 0;
    for (const auto& p : paths) {
        struct stat st;
        if (stat(p.c_str(), &st) == 0 && st.st_mtime > newest) newest = st.st_mtime;
    }
    if (newest == 0) return;

    double days = difftime(time(nullptr), newest) / 86400.0;
    if (days > static_cast<double>(max_age_days)) {
        std::cerr << "Note: the list is " << static_cast<long>(days)
                  << " days old, run with --update for newer ones\n";
    }
}

static void search(const std::vector<Source>& sources,
                   const std::vector<std::regex>& patterns,
                   const std::vector<std::string>& queries,
                   const SearchOptions& so,
                   const std::vector<std::string>& age_paths) {
    std::vector<Match> matches;
    std::vector<std::string> pool;

    for (const auto& src : sources) {
        for (const auto& item : src.items) {
            const std::string& name = item.first;
            pool.push_back(name);

            bool hit = true;
            if (so.exact) {
                hit = exact_name(name, patterns);
            } else {
                for (const auto& p : patterns) {
                    if (!std::regex_search(name, p)) { hit = false; break; }
                }
            }
            if (!hit) continue;

            Match candidate{name, src.label, item.second, ""};
            if (!src.base_url.empty())
                candidate.url = src.base_url + "/" + name + ".tgz";

            if (so.installed_only && !is_installed(candidate, so.installed))
                continue;

            matches.push_back(std::move(candidate));
        }
    }

    std::vector<std::string> suggestions;
    if (matches.empty() && queries.size() == 1 && !so.no_suggest && !so.installed_only)
        suggestions = suggest(pool, queries[0], 5);

    if (!suggestions.empty()) {
        if (so.json) print_json(matches, so, suggestions);
        else print_text(matches, so, suggestions);
        return;
    }

    matches = hide_superseded(matches);
    if (so.exact || so.collapse_newest)
        matches = keep_newest(matches, so.stem_only);

    std::sort(matches.begin(), matches.end(), match_less);

    if (so.limit > 0 && static_cast<long>(matches.size()) > so.limit)
        matches.resize(static_cast<size_t>(so.limit));

    if (so.json) print_json(matches, so, suggestions);
    else print_text(matches, so, suggestions);

    warn_if_stale(age_paths, so.max_age_days);
}

// ---------------------------------------------------------------------------
// Argument parsing
// ---------------------------------------------------------------------------

struct Options {
    std::vector<std::string> queries;
    std::string os;
    bool sha256 = false;
    bool sha512 = false;
    bool update = false;
    std::string release;
    std::string arch;
    std::string mirror;
    bool list_releases = false;
    bool exact = false;
    bool json = false;
    bool verbose = false;
    bool installed = false;
    bool installed_only = false;
    bool show_url = false;
    bool no_suggest = false;
    bool fixed_strings = false;
    bool newest = false;
    bool clean = false;
    long limit = -1;
    long max_age_days = 7;
    long timeout = 120;
    bool error = false;
};

static bool valid_release(const std::string& v) {
    // NetBSD releases may contain an uppercase quarter, e.g. 10.0_2026Q3.
    static const std::regex re(R"(\d+\.\d+(?:_[A-Za-z0-9]+)?|snapshots)");
    return std::regex_match(v, re);
}

static bool valid_arch(const std::string& v) {
    // x86_64 contains an underscore.
    static const std::regex re(R"([A-Za-z0-9_]+)");
    return std::regex_match(v, re);
}

static void print_help(const std::string& prog) {
    std::cout <<
        "usage: " << prog << " [options] [package ...]\n\n"
        "Search OpenBSD and NetBSD package lists from a local cache.\n\n"
        "Cached lists are used when available.  The network is contacted only\n"
        "when no suitable cache exists or when --update is given.\n\n"
        "positional arguments:\n"
        "  package                 words to search for, case-insensitive; a\n"
        "                          package must contain all of them. A word with\n"
        "                          any of ^$*?()[]{}|\\ is a regular expression\n"
        "                          unless -F is used\n\n"
        "options:\n"
        "  -h, --help              show this help message and exit\n"
        "  --version               show the version and exit\n"
        "  --os {openbsd,netbsd}   which package universe to search\n"
        "                          (default: auto-detect)\n"
        "  --release RELEASE       release to search; fetched if not cached\n"
        "  --arch ARCH             package architecture, downloaded if needed\n"
        "  --list-releases         list releases (cached unless --update)\n"
        "  -u, --update            refresh cached package lists from the mirror\n"
        "  --mirror URL            override the package mirror\n"
        "  --cache-dir DIR         override the cache directory\n"
        "  --clean                 remove the cache and exit\n"
        "  --sha256, --sha512      show each package's digest (SHA256 on\n"
        "                          OpenBSD, SHA512 on NetBSD)\n"
        "  --url                   show the package download URL\n"
        "  -j, --json              print machine-readable JSON\n"
        "  --limit N               show at most N results\n"
        "  --installed             mark packages that are installed locally\n"
        "  --installed-only        show only packages installed locally\n"
        "  --newest                show only the newest version of each line\n"
        "  --max-age DAYS          warn when the cached list is older than DAYS\n"
        "                          (default: 7; 0 disables the warning)\n"
        "  --timeout SECONDS       network timeout (default: 120)\n"
        "  -e, --exact             match whole package names and show newest\n"
        "  -F, --fixed-strings     treat search words as literal strings\n"
        "  --no-suggest            do not print \"Did you mean?\" suggestions\n"
        "  -v, --verbose           print extra diagnostics\n\n"
        "Package lists are saved under " << g_cache_root << ".\n"
        "Examples:\n"
        "  " << prog << " firefox\n"
        "  " << prog << " -e php --newest --sha256\n"
        "  " << prog << " --os netbsd --json --limit 20 jdk\n";
}

static Options parse_args(int argc, char** argv, const std::string& prog) {
    Options a;
    bool end_of_options = false;

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (end_of_options) {
            a.queries.push_back(arg);
            continue;
        }
        if (arg == "--") {
            end_of_options = true;
            continue;
        }

        std::string inline_value;
        bool has_inline = false;
        if (arg.size() > 2 && arg[0] == '-' && arg[1] == '-') {
            size_t eq = arg.find('=');
            if (eq != std::string::npos) {
                inline_value = arg.substr(eq + 1);
                arg = arg.substr(0, eq);
                has_inline = true;
            }
        }

        if (arg == "-h" || arg == "--help") {
            print_help(prog);
            std::exit(0);
        }
        if (arg == "--version") {
            std::cout << "0verbs " << PROGRAM_VERSION << "\n";
            std::exit(0);
        }

        auto value = [&](const char* name, std::string& out) -> bool {
            if (has_inline) {
                out = inline_value;
                has_inline = false;
                return true;
            }
            if (i + 1 >= argc) {
                std::cerr << "error: " << name << " requires a value\n";
                a.error = true;
                return false;
            }
            out = argv[++i];
            return true;
        };

        auto number = [&](const char* name, long min_value, long& out) -> bool {
            std::string v;
            if (!value(name, v)) return false;
            try {
                size_t pos = 0;
                long n = std::stol(v, &pos);
                if (pos != v.size() || n < min_value || n > 1000000)
                    throw std::invalid_argument("out of range");
                out = n;
            } catch (...) {
                std::cerr << "error: " << name << " expects an integer >= "
                          << min_value << "\n";
                a.error = true;
                return false;
            }
            return true;
        };

        if (arg == "--os") {
            if (value("--os", a.os)) {
                a.os = to_lower(trim(a.os));
                if (a.os != "openbsd" && a.os != "netbsd") {
                    std::cerr << "error: --os must be openbsd or netbsd\n";
                    a.error = true;
                }
            }
        } else if (arg == "--sha256") {
            a.sha256 = true;
        } else if (arg == "--sha512") {
            a.sha512 = true;
        } else if (arg == "-u" || arg == "--update") {
            a.update = true;
        } else if (arg == "--release") {
            if (value("--release", a.release)) {
                a.release = trim(a.release);
                if (!valid_release(a.release)) {
                    std::cerr << "error: release '" << a.release
                              << "' is not like 7.8, snapshots or 11.0_2026Q3\n";
                    a.error = true;
                }
            }
        } else if (arg == "--arch") {
            if (value("--arch", a.arch)) {
                a.arch = to_lower(trim(a.arch));
                if (!valid_arch(a.arch)) {
                    std::cerr << "error: arch '" << a.arch
                              << "' is not like amd64 or x86_64\n";
                    a.error = true;
                }
            }
        } else if (arg == "--mirror") {
            if (value("--mirror", a.mirror)) {
                a.mirror = strip_trailing_slashes(trim(a.mirror));
                if (!valid_mirror(a.mirror)) {
                    std::cerr << "error: --mirror must start with http:// or https://\n";
                    a.error = true;
                }
            }
        } else if (arg == "--cache-dir") {
            if (value("--cache-dir", g_cache_root)) {
                g_cache_root = strip_trailing_slashes(expand_home(trim(g_cache_root)));
                if (g_cache_root.empty()) {
                    std::cerr << "error: --cache-dir must not be empty\n";
                    a.error = true;
                }
            }
        } else if (arg == "--list-releases") {
            a.list_releases = true;
        } else if (arg == "-e" || arg == "--exact") {
            a.exact = true;
        } else if (arg == "-j" || arg == "--json") {
            a.json = true;
        } else if (arg == "--url") {
            a.show_url = true;
        } else if (arg == "--limit") {
            number("--limit", 1, a.limit);
        } else if (arg == "--max-age") {
            number("--max-age", 0, a.max_age_days);
        } else if (arg == "--timeout") {
            number("--timeout", 1, a.timeout);
        } else if (arg == "--installed") {
            a.installed = true;
        } else if (arg == "--installed-only") {
            a.installed = true;
            a.installed_only = true;
        } else if (arg == "--newest") {
            a.newest = true;
        } else if (arg == "--no-suggest") {
            a.no_suggest = true;
        } else if (arg == "-F" || arg == "--fixed-strings") {
            a.fixed_strings = true;
        } else if (arg == "--clean") {
            a.clean = true;
        } else if (arg == "-v" || arg == "--verbose") {
            a.verbose = true;
        } else if (!arg.empty() && arg[0] == '-' && arg != "-") {
            std::cerr << "error: unrecognized argument: " << arg << "\n";
            a.error = true;
            has_inline = false;
        } else {
            a.queries.push_back(arg);
        }

        if (has_inline) {
            std::cerr << "error: option " << arg << " does not take a value\n";
            a.error = true;
            has_inline = false;
        }
    }

    return a;
}

// ---------------------------------------------------------------------------
// Cache maintenance
// ---------------------------------------------------------------------------

static bool is_safe_cache_path(const fs::path& p) {
    if (p.empty() || p.filename().empty()) return false;

    std::error_code ec;
    fs::path abs = fs::weakly_canonical(p, ec);
    if (ec) abs = fs::absolute(p, ec);
    if (ec || abs.empty() || abs == abs.root_path()) return false;

    std::error_code cec;
    if (abs == fs::current_path(cec) && !cec) return false;

    auto same_path = [&](const fs::path& q) {
        std::error_code e;
        fs::path qa = fs::weakly_canonical(q, e);
        if (e) qa = fs::absolute(q, e);
        return !e && !qa.empty() && abs == qa;
    };

    // Never remove a home directory, however it was spelled.
    for (const char* var : {"HOME", "USERPROFILE"}) {
        const char* v = std::getenv(var);
        if (v && *v && same_path(fs::path(v))) return false;
    }

    // Only the cache directory itself, something beneath the default cache
    // root, or a directory that holds nothing but openbsd/ and netbsd/ may be
    // deleted.  The name alone is not enough: a git clone is also "0verbs".
    fs::path def = fs::weakly_canonical(fs::path(default_cache_root()), ec);
    if (!ec) {
        fs::path rel = abs.lexically_relative(def);
        if (abs == def || (!rel.empty() && rel.begin()->string() != "..")) return true;
    }
    std::error_code iec;
    for (fs::directory_iterator it(abs, iec), end; !iec && it != end; it.increment(iec)) {
        std::string name = it->path().filename().string();
        if (name != "openbsd" && name != "netbsd") return false;
    }
    return !iec;
}

static bool clean_cache() {
    std::error_code ec;
    if (!fs::exists(g_cache_root, ec)) {
        std::cout << "Cache is empty: " << g_cache_root << "\n";
        return true;
    }
    if (!is_safe_cache_path(fs::path(g_cache_root))) {
        std::cerr << "ERROR: refusing to remove unsafe cache path: " << g_cache_root
                  << "\nThe cache directory must hold only openbsd/ and netbsd/, or live under "
                  << default_cache_root() << "\n";
        return false;
    }

    std::uintmax_t n = fs::remove_all(g_cache_root, ec);
    if (ec) {
        std::cerr << "ERROR: could not clean " << g_cache_root << ": " << ec.message() << "\n";
        return false;
    }
    std::cout << "Removed " << n << " entries from " << g_cache_root << "\n";
    return true;
}

// ---------------------------------------------------------------------------
// Backends
// ---------------------------------------------------------------------------

static int run_openbsd(const Options& opt) {
    const std::string mirror = openbsd_mirror(opt.mirror);
    const std::string arch = normalize_arch(
        OPENBSD_ARCH_NAMES, opt.arch.empty() ? openbsd_default_arch() : opt.arch);

    if (opt.list_releases) {
        std::vector<std::string> releases;
        if (!opt.update) releases = cached_releases("openbsd", arch);
        if (releases.empty()) {
            std::string error;
            if (!openbsd_mirror_releases(mirror, opt.timeout, releases, error)) {
                std::cerr << "ERROR: " << error << "\n";
                return 1;
            }
        }
        print_releases(releases, opt.json);
        return 0;
    }

    if (opt.queries.empty() && !opt.update) {
        std::cerr << "ERROR: Please supply some package!\n";
        return 1;
    }

    std::string release = opt.release;
    if (release.empty()) {
        // On OpenBSD the installed release (or snapshots on -current/-beta)
        // wins without asking the mirror.  Elsewhere keep the cache-first
        // behaviour and fall back to the newest release on the mirror.
        if (!opt.update && host_sysname() != "OpenBSD")
            release = latest_cached_release("openbsd", arch);
        if (release.empty() && !openbsd_default_release(mirror, opt.timeout, release))
            return 1;
    }

    std::vector<std::string> need;
    for (const auto& folder : OPENBSD_FOLDERS) {
        if (opt.update || !file_exists(local_list("openbsd", release, arch, folder)))
            need.push_back(folder);
    }

    for (const auto& folder : need) {
        if (!openbsd_download(mirror, release, arch, folder, opt.timeout))
            return 1;
    }

    if (opt.queries.empty()) return 0;  // --update without a search

    std::vector<std::regex> patterns;
    for (const auto& word : opt.queries) {
        std::regex re;
        std::string error;
        if (!compile_word(word, opt.fixed_strings, re, error)) {
            std::cerr << "ERROR: " << error << "\n";
            std::cerr << "Hint: use -F/--fixed-strings for a literal search.\n";
            return 1;
        }
        patterns.push_back(std::move(re));
    }

    std::vector<Source> sources;
    for (const auto& folder : OPENBSD_FOLDERS) {
        std::string db = local_list("openbsd", release, arch, folder);
        Source src;
        src.label = (folder == "packages-stable") ? "stable" : "";
        src.base_url = mirror + "/" + release + "/" + folder + "/" + arch;
        src.items = openbsd_parse(db);
        if (opt.verbose)
            std::cerr << "[verbose] " << db << ": " << src.items.size() << " packages\n";
        sources.push_back(std::move(src));
    }

    SearchOptions so;
    so.show_hash = opt.sha256 || opt.sha512;
    so.exact = opt.exact;
    so.json = opt.json;
    so.show_url = opt.show_url;
    so.annotate_installed = opt.installed;
    so.installed_only = opt.installed_only;
    so.no_suggest = opt.no_suggest;
    so.collapse_newest = opt.exact || opt.newest || opt.installed_only;
    so.stem_only = false;
    so.limit = opt.limit;
    so.max_age_days = opt.max_age_days;
    so.hash_label = "SHA256";
    so.os = "openbsd";
    so.release = release;
    so.arch = arch;

    InstalledPackages installed;
    if (opt.installed || opt.installed_only) {
        installed = read_installed("openbsd");
        so.installed = &installed;
        if (opt.verbose)
            std::cerr << "[verbose] installed packages: " << installed.names.size() << "\n";
    }

    search(sources, patterns, opt.queries, so,
           {local_list("openbsd", release, arch, "packages"),
            local_list("openbsd", release, arch, "packages-stable")});
    return 0;
}

static int run_netbsd(const Options& opt) {
    const std::string mirror = netbsd_mirror(opt.mirror);
    const std::string arch = normalize_arch(
        NETBSD_ARCH_NAMES, opt.arch.empty() ? netbsd_default_arch() : opt.arch);

    if (opt.list_releases) {
        std::vector<std::string> releases;
        if (!opt.update) releases = cached_releases("netbsd", arch);
        if (releases.empty()) {
            std::string error;
            if (!netbsd_mirror_releases(mirror, arch, opt.timeout, releases, error)) {
                std::cerr << "ERROR: " << error << "\n";
                return 1;
            }
        }
        print_releases(releases, opt.json);
        return 0;
    }

    if (opt.queries.empty() && !opt.update) {
        std::cerr << "ERROR: Please supply some package!\n";
        return 1;
    }

    std::string release = opt.release;
    if (release.empty()) {
        // On NetBSD prefer a cached release with the host's major version;
        // anywhere else keep the cache-first behaviour.  Either way, fall
        // back to the mirror when nothing suitable is cached.
        if (!opt.update) {
            if (host_sysname() == "NetBSD") {
                std::string major = host_major_prefix();
                if (!major.empty())
                    release = latest_cached_major("netbsd", arch, major);
            } else {
                release = latest_cached_release("netbsd", arch);
            }
        }
        if (release.empty() && !netbsd_default_release(mirror, arch, opt.timeout, release))
            return 1;
    }

    std::string db = local_list("netbsd", release, arch, "packages", "SHA512");

    if (opt.update || !file_exists(db)) {
        if (!netbsd_download(mirror, release, arch, opt.timeout)) return 1;
    }

    if (opt.queries.empty()) return 0;  // --update without a search

    std::vector<std::regex> patterns;
    for (const auto& word : opt.queries) {
        std::regex re;
        std::string error;
        if (!compile_word(word, opt.fixed_strings, re, error)) {
            std::cerr << "ERROR: " << error << "\n";
            std::cerr << "Hint: use -F/--fixed-strings for a literal search.\n";
            return 1;
        }
        patterns.push_back(std::move(re));
    }

    Source src;
    src.label = "";
    src.base_url = mirror + "/" + arch + "/" + release + "/All";
    src.items = netbsd_parse(db);
    if (opt.verbose)
        std::cerr << "[verbose] " << db << ": " << src.items.size() << " packages\n";

    SearchOptions so;
    so.show_hash = opt.sha512 || opt.sha256;
    so.exact = opt.exact;
    so.json = opt.json;
    so.show_url = opt.show_url;
    so.annotate_installed = opt.installed;
    so.installed_only = opt.installed_only;
    so.no_suggest = opt.no_suggest;
    so.collapse_newest = true;  // NetBSD has always shown one row per stem
    so.stem_only = true;
    so.limit = opt.limit;
    so.max_age_days = opt.max_age_days;
    so.hash_label = "SHA512";
    so.os = "netbsd";
    so.release = release;
    so.arch = arch;

    InstalledPackages installed;
    if (opt.installed || opt.installed_only) {
        installed = read_installed("netbsd");
        so.installed = &installed;
        if (opt.verbose)
            std::cerr << "[verbose] installed packages: " << installed.names.size() << "\n";
    }

    search({src}, patterns, opt.queries, so, {db});
    return 0;
}

// ---------------------------------------------------------------------------
// Main
// ---------------------------------------------------------------------------

struct CurlGlobal {
    bool ok = false;
    CurlGlobal() {
        ok = curl_global_init(CURL_GLOBAL_DEFAULT) == CURLE_OK;
    }
    ~CurlGlobal() {
        if (ok) curl_global_cleanup();
    }
};

int main(int argc, char* argv[]) {
    const std::string prog = fs::path(argv[0]).filename().string();
    g_cache_root = default_cache_root();

    Options opt = parse_args(argc, argv, prog);
    if (opt.error) return EXIT_FAILURE;

    if (opt.clean) return clean_cache() ? 0 : EXIT_FAILURE;

    CurlGlobal curl;
    if (!curl.ok) {
        std::cerr << "ERROR: could not initialise libcurl\n";
        return EXIT_FAILURE;
    }

    const std::string target = opt.os.empty() ? detect_os() : opt.os;
    const int rc = (target == "openbsd") ? run_openbsd(opt) : run_netbsd(opt);
    return rc;
}