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
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <optional>
#include <regex>
#include <set>
#include <sstream>
#include <string>
#include <sys/stat.h>
#include <sys/utsname.h>
#include <unistd.h>
#include <vector>

#include <curl/curl.h>
#include <bzlib.h>

namespace fs = std::filesystem;

// ---------------------------------------------------------------------------
// Configuration
// ---------------------------------------------------------------------------

static std::string g_cache_root;

static const char* INSTALLURL = "/etc/installurl";
static const char* OPENBSD_DEFAULT_MIRROR = "https://cdn.openbsd.org/pub/OpenBSD";
static const std::vector<std::string> OPENBSD_FOLDERS = {"packages", "packages-stable"};
static const char* NETBSD_DEFAULT_MIRROR = "https://cdn.netbsd.org/pub/pkgsrc/packages/NetBSD";

static const std::map<std::string, std::string> OPENBSD_ARCH_NAMES = {
    {"x86_64", "amd64"}, {"amd64", "amd64"},
    {"arm64", "aarch64"}, {"aarch64", "aarch64"},
    {"i386", "i386"}, {"i486", "i386"}, {"i586", "i386"}, {"i686", "i386"}, {"x86", "i386"},
    {"armv7l", "arm"}, {"armv7", "arm"},
    {"macppc", "powerpc"},
    {"octeon", "mips64"},
    {"loongson", "mips64el"},
    {"riscv64", "riscv64"},
    {"ppc64", "powerpc64"}, {"powerpc64", "powerpc64"},
    {"sparc64", "sparc64"},
    {"mips64", "mips64"},
};

static const std::map<std::string, std::string> NETBSD_ARCH_NAMES = {
    {"x86_64", "amd64"}, {"amd64", "amd64"},
    {"i386", "i386"}, {"i486", "i386"}, {"i586", "i386"}, {"i686", "i386"}, {"x86", "i386"},
    {"arm64", "aarch64"}, {"aarch64", "aarch64"},
    {"armv7l", "earmv7hf"}, {"armv7", "earmv7hf"},
    {"macppc", "macppc"},
    {"riscv64", "riscv64"},
    {"sparc64", "sparc64"},
    {"mips64", "mips64"},
};

static const std::regex PACKAGE_NAME(R"((.+?)-(\d[^-]*?)(?:p(\d+))?(?:v(\d+))?(?:nb(\d+))?((?:-[^-]+)*)$)");

static const std::string REGEX_SYNTAX = "^$*?()[]{}|\\";

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

static std::string to_lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return (char)std::tolower(c); });
    return s;
}

static std::string trim(const std::string& s) {
    size_t a = s.find_first_not_of(" \t\r\n");
    if (a == std::string::npos) return "";
    size_t b = s.find_last_not_of(" \t\r\n");
    return s.substr(a, b - a + 1);
}

struct VersionOrder {
    std::vector<std::string> parts;
    bool operator<(const VersionOrder& o) const {
        size_t n = std::min(parts.size(), o.parts.size());
        for (size_t i = 0; i < n; ++i) {
            const std::string& a = parts[i];
            const std::string& b = o.parts[i];
            bool na = !a.empty() && std::all_of(a.begin(), a.end(), ::isdigit);
            bool nb = !b.empty() && std::all_of(b.begin(), b.end(), ::isdigit);
            if (na && nb) {
                long long ia = std::atoll(a.c_str());
                long long ib = std::atoll(b.c_str());
                if (ia != ib) return ia < ib;
            } else if (na != nb) {
                return na;
            } else {
                if (a != b) return a < b;
            }
        }
        return parts.size() < o.parts.size();
    }
};

static VersionOrder version_order(const std::string& name) {
    VersionOrder v;
    std::string cur;
    for (char c : name) {
        if (std::isdigit((unsigned char)c)) {
            cur.push_back(c);
        } else {
            if (!cur.empty()) { v.parts.push_back(cur); cur.clear(); }
            v.parts.push_back(std::string(1, c));
        }
    }
    if (!cur.empty()) v.parts.push_back(cur);
    return v;
}

static std::string detect_os() {
    struct utsname u;
    uname(&u);
    if (std::string(u.sysname) == "OpenBSD") return "openbsd";
    return "netbsd";
}

// ---------------------------------------------------------------------------
// HTTP fetch via libcurl
// ---------------------------------------------------------------------------

struct CurlWrite { std::string* out; };

static size_t curl_write_cb(void* ptr, size_t size, size_t nmemb, void* userdata) {
    auto* w = static_cast<CurlWrite*>(userdata);
    w->out->append(static_cast<char*>(ptr), size * nmemb);
    return size * nmemb;
}

static bool fetch(const std::string& url, std::string& out, bool announce = false, bool missing_ok = false) {
    if (announce) std::cout << "Downloading " << url << " ... " << std::flush;
    CURL* curl = curl_easy_init();
    if (!curl) {
        if (announce) std::cout << "\n";
        std::cerr << "ERROR: could not init curl\n";
        return false;
    }
    out.clear();
    CurlWrite w{&out};
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, curl_write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &w);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 120L);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    CURLcode res = curl_easy_perform(curl);
    long code = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &code);
    curl_easy_cleanup(curl);

    if (announce) std::cout << "\n";
    if (res != CURLE_OK) {
        std::cerr << "ERROR: could not download " << url << ": "
                  << curl_easy_strerror(res) << "\n";
        return false;
    }
    if (code >= 400) {
        if (missing_ok && code == 404) { out.clear(); return true; }
        std::cerr << "ERROR: HTTP " << code << " for " << url << "\n";
        if (code == 404) {
            std::cerr << "Use --release and/or --arch to pick a release and "
                         "architecture that exist on the mirror\n";
        }
        return false;
    }
    return true;
}

// ---------------------------------------------------------------------------
// Cache paths
// ---------------------------------------------------------------------------

static std::string local_list(const std::string& flavour, const std::string& release, const std::string& arch, const std::string& folder = "packages", const std::string& filename = "SHA256") {
    return (fs::path(g_cache_root) / flavour / release / folder / arch / filename).string();
}

static void ensure_parent(const std::string& path) {
    fs::path p(path);
    if (p.has_parent_path()) fs::create_directories(p.parent_path());
}

static void atomic_write(const std::string& path, const std::string& data) {
    ensure_parent(path);
    std::string tmp = path + ".tmp";
    std::ofstream f(tmp, std::ios::binary);
    f.write(data.data(), (std::streamsize)data.size());
    f.close();
    fs::rename(tmp, path);
}

// ---------------------------------------------------------------------------
// OpenBSD backend
// ---------------------------------------------------------------------------

static std::string openbsd_mirror() {
    std::ifstream f(INSTALLURL);
    std::string url;
    if (f) {
        std::getline(f, url);
        url = trim(url);
        while (!url.empty() && url.back() == '/') url.pop_back();
    }
    return url.empty() ? std::string(OPENBSD_DEFAULT_MIRROR) : url;
}

static std::vector<std::string> openbsd_mirror_releases(const std::string& mirror) {
    std::string listing;
    if (!fetch(mirror + "/", listing)) return {};
    std::regex re(R"(href="(\d+\.\d+)/")");
    std::vector<std::string> out;
    auto begin = std::sregex_iterator(listing.begin(), listing.end(), re);
    for (auto it = begin; it != std::sregex_iterator(); ++it)
        out.push_back((*it)[1]);
    std::sort(out.begin(), out.end(), [](const std::string& a, const std::string& b) { return version_order(a) < version_order(b); });
    return out;
}

static std::string openbsd_default_release(const std::string& mirror) {
    auto releases = openbsd_mirror_releases(mirror);
    if (releases.empty()) {
        std::cerr << "ERROR: could not list releases at " << mirror << "/\n";
        std::exit(1);
    }
    return releases.back();
}

static std::string openbsd_default_arch() {
    struct utsname u;
    uname(&u);
    std::string machine = to_lower(u.machine);
    auto it = OPENBSD_ARCH_NAMES.find(machine);
    return it != OPENBSD_ARCH_NAMES.end() ? it->second : machine;
}

static bool openbsd_download(const std::string& mirror, const std::string& release, const std::string& arch, const std::string& folder) {
    std::string url = mirror + "/" + release + "/" + folder + "/" + arch + "/SHA256";
    std::string db = local_list("openbsd", release, arch, folder);
    std::string data;
    bool missing_ok = (folder != "packages");
    if (!fetch(url, data, true, missing_ok)) return false;
    if (data.empty() && folder == "packages") {
        std::cerr << "ERROR: empty listing for " << url << "\n";
        return false;
    }
    atomic_write(db, data);
    if (data.empty()) std::cout << "no updates yet\n";
    else std::cout << "saved to " << db << "\n";
    return true;
}

static std::vector<std::pair<std::string, std::string>>
openbsd_parse(const std::string& db_path) {
    std::vector<std::pair<std::string, std::string>> out;
    std::ifstream f(db_path);
    if (!f) return out;
    std::string line;
    std::regex re(R"(SHA256 \((?:All/)?(.+?)\.tgz\) = ([0-9a-fA-F]+))");
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

static std::string netbsd_mirror() { return NETBSD_DEFAULT_MIRROR; }

static std::vector<std::string> netbsd_mirror_releases(const std::string& mirror, const std::string& arch) {
    std::string listing;
    if (!fetch(mirror + "/" + arch + "/", listing)) return {};
    std::regex re(R"(href="(\d+\.\d+_[^/"]+)/")");
    std::vector<std::string> out;
    auto begin = std::sregex_iterator(listing.begin(), listing.end(), re);
    for (auto it = begin; it != std::sregex_iterator(); ++it)
        out.push_back((*it)[1]);
    std::sort(out.begin(), out.end(), [](const std::string& a, const std::string& b) { return version_order(a) < version_order(b); });
    return out;
}

static std::string netbsd_default_release(const std::string& mirror, const std::string& arch) {
    auto releases = netbsd_mirror_releases(mirror, arch);
    if (releases.empty()) {
        std::cerr << "ERROR: could not list releases for arch " << arch
                  << " at " << mirror << "/" << arch << "/\n";
        std::exit(1);
    }
    return releases.back();
}

static std::string netbsd_default_arch() {
    struct utsname u;
    uname(&u);
    std::string machine = to_lower(u.machine);
    auto it = NETBSD_ARCH_NAMES.find(machine);
    return it != NETBSD_ARCH_NAMES.end() ? it->second : machine;
}

static bool decompress_bz2(const std::string& in, std::string& out) {
    bz_stream strm{};
    if (BZ2_bzDecompressInit(&strm, 0, 0) != BZ_OK) return false;
    out.clear();
    char buf[16384];
    strm.next_in = const_cast<char*>(in.data());
    strm.avail_in = (unsigned)in.size();
    int ret;
    do {
        strm.next_out = buf;
        strm.avail_out = sizeof(buf);
        ret = BZ2_bzDecompress(&strm);
        if (ret != BZ_OK && ret != BZ_STREAM_END) {
            BZ2_bzDecompressEnd(&strm);
            return false;
        }
        out.append(buf, sizeof(buf) - strm.avail_out);
    } while (ret != BZ_STREAM_END);
    BZ2_bzDecompressEnd(&strm);
    return true;
}

static bool netbsd_download(const std::string& mirror, const std::string& release, const std::string& arch) {
    std::string url = mirror + "/" + arch + "/" + release + "/SHA512.bz2";
    std::string raw_db = local_list("netbsd", release, arch, "packages", "SHA512.bz2");
    std::string clean_db = local_list("netbsd", release, arch, "packages", "SHA512");
    std::string data;
    if (!fetch(url, data, true)) return false;

    std::string text;
    if (!decompress_bz2(data, text)) {
        std::cerr << "ERROR: could not decompress bzip2 data\n";
        return false;
    }
    atomic_write(raw_db, data);

    std::istringstream in(text);
    std::ostringstream out;
    std::string line;
    std::regex re(R"((SHA512 \()All/(.+\.tgz\) = .*))");
    while (std::getline(in, line)) {
        std::smatch m;
        if (std::regex_match(line, m, re))
            out << m[1] << m[2] << "\n";
        else
            out << line << "\n";
    }
    atomic_write(clean_db, out.str());
    std::cout << "saved raw to " << raw_db << "\n";
    std::cout << "saved cleaned list to " << clean_db << "\n";
    return true;
}

static std::vector<std::pair<std::string, std::string>>
netbsd_parse(const std::string& db_path) {
    std::vector<std::pair<std::string, std::string>> out;
    std::ifstream f(db_path);
    if (!f) return out;
    std::string line;
    std::regex re(R"(SHA512 \((.+?)\.tgz\) = ([A-Fa-f0-9]+))");
    while (std::getline(f, line)) {
        std::smatch m;
        if (std::regex_search(line, m, re))
            out.emplace_back(m[1], m[2]);
    }
    return out;
}

// ---------------------------------------------------------------------------
// Shared search / output
// ---------------------------------------------------------------------------

static std::regex word_pattern(const std::string& word) {
    bool has_syntax = false;
    for (char c : word)
        if (REGEX_SYNTAX.find(c) != std::string::npos) { has_syntax = true; break; }
    if (has_syntax) return std::regex(word, std::regex::icase);
    std::string esc;
    for (char c : word) {
        if (std::string(".^$|()[]{}*+?\\").find(c) != std::string::npos) esc.push_back('\\');
        esc.push_back(c);
    }
    return std::regex(esc, std::regex::icase);
}

struct PackageParts {
    std::string stem, version, flavor;
    int p = -1, v = -1, nb = -1;
    bool ok = false;
};

static PackageParts parse_package(const std::string& name) {
    PackageParts pp;
    std::smatch m;
    if (std::regex_match(name, m, PACKAGE_NAME)) {
        pp.stem = m[1];
        pp.version = m[2];
        if (m[3].matched) pp.p = std::stoi(m[3]);
        if (m[4].matched) pp.v = std::stoi(m[4]);
        if (m[5].matched) pp.nb = std::stoi(m[5]);
        pp.flavor = m[6];
        pp.ok = true;
    }
    return pp;
}

static bool exact_name(const std::string& name, const std::vector<std::regex>& patterns) {
    auto parts = parse_package(name);
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
};

static std::vector<Match> keep_newest(const std::vector<Match>& matches, bool stem_only = false) {
    // Rank is exactly the Python tuple:
    //   (int(v or 0), version_order(version), int(p or -1), int(nb or -1))
    // std::tuple's operator> gives lexicographic comparison, matching Python.
    using Rank = std::tuple<int, VersionOrder, int, int>;

    struct Key {
        std::string stem;
        std::string flavor;
        std::string firstdigit;
        bool operator<(const Key& o) const {
            if (stem != o.stem) return stem < o.stem;
            if (flavor != o.flavor) return flavor < o.flavor;
            return firstdigit < o.firstdigit;
        }
    };

    std::map<Key, std::pair<Rank, Match>> newest;

    for (const auto& match : matches) {
        PackageParts parts = parse_package(match.name);

        if (!parts.ok) {
            // Unparseable name: keep one entry per literal name.
            Key k{match.name, "", ""};
            if (!newest.count(k)) {
                newest[k] = {Rank{0, VersionOrder{}, -1, -1}, match};
            }
            continue;
        }

        Key k;
        if (stem_only) {
            // NetBSD: collapse by stem only (jdk-17 and jdk-21 become one row).
            k = {parts.stem, "", ""};
        } else {
            // OpenBSD: keep php-8.3 and php-8.4 separate, but collapse
            // php-8.4.1 and php-8.4.2.
            std::smatch vm;
            std::string firstdigit;
            if (std::regex_search(parts.version, vm, std::regex(R"(^\d+)"))) {
                firstdigit = vm[0];
            }
            k = {parts.stem, parts.flavor, firstdigit};
        }

        Rank rank{
            parts.v,                       // epoch (vN) or 0
            version_order(parts.version),  // numeric-aware version
            parts.p,                       // pkgfix pN or -1
            parts.nb                       // pkgsrc revision nbN or -1
        };

        auto it = newest.find(k);
        if (it == newest.end() || rank > it->second.first) {
            newest[k] = {rank, match};
        }
    }

    std::vector<Match> out;
    out.reserve(newest.size());
    for (auto& kv : newest) out.push_back(kv.second.second);
    return out;
}

static std::vector<Match> hide_superseded(const std::vector<Match>& matches) {
    auto line_key = [](const std::string& name) -> std::optional<std::string> {
        auto parts = parse_package(name);
        if (!parts.ok) return std::nullopt;
        std::smatch vm;
        std::regex_search(parts.version, vm, std::regex(R"(^\d+)"));
        return parts.stem + "\x01" + parts.flavor + "\x01" + vm[0].str();
    };
    std::set<std::string> updated;
    for (const auto& m : matches) {
        if (!m.label.empty()) {
            auto k = line_key(m.name);
            if (k) updated.insert(*k);
        }
    }
    std::vector<Match> out;
    for (const auto& m : matches) {
        if (!m.label.empty()) { out.push_back(m); continue; }
        auto k = line_key(m.name);
        if (!k || !updated.count(*k)) out.push_back(m);
    }
    return out;
}

static void search(const std::vector<std::pair<std::string,
                   std::vector<std::pair<std::string, std::string>>>>& sources,
                   const std::vector<std::regex>& patterns,
                   const std::vector<std::string>& queries,
                   bool show_sha, bool exact,
                   const std::vector<std::string>& age_paths,
                   const std::string& hash_label, bool always_newest) {
    std::vector<Match> matches;
    std::vector<std::string> suggestions_pool;
    for (const auto& src : sources) {
        for (const auto& item : src.second) {
            const std::string& name = item.first;
            suggestions_pool.push_back(name);
            bool hit = exact ? exact_name(name, patterns) : true;
            if (!exact) {
                for (const auto& p : patterns) {
                    if (!std::regex_search(name, p)) { hit = false; break; }
                }
            }
            if (hit) matches.push_back({name, src.first, item.second});
        }
    }

    if (matches.empty() && queries.size() == 1) {
        std::string q = to_lower(queries[0]);
        struct Cand { std::string name; double score; };
        std::vector<Cand> cands;
        for (const auto& name : suggestions_pool) {
            std::string n = to_lower(name);
            size_t common = 0;
            size_t lim = std::min(q.size(), n.size());
            while (common < lim && q[common] == n[common]) ++common;
            double score = (double)common / (double)std::max(q.size(), n.size());
            if (score >= 0.4) cands.push_back({name, score});
        }
        std::sort(cands.begin(), cands.end(),
                  [](const Cand& a, const Cand& b) { return a.score > b.score; });
        if (!cands.empty()) {
            std::cout << "\nDid you mean?\n";
            for (size_t i = 0; i < std::min<size_t>(5, cands.size()); ++i)
                std::cout << "  " << cands[i].name << "\n";
            return;
        }
    }

    matches = hide_superseded(matches);
    if (exact || always_newest) matches = keep_newest(matches, always_newest);

    std::sort(matches.begin(), matches.end(), [](const Match& a, const Match& b) {
        auto a_stem = a.name.substr(0, a.name.find("-"));
        auto b_stem = b.name.substr(0, b.name.find("-"));
        if (a_stem != b_stem) return a_stem < b_stem;
        return version_order(a.name) < version_order(b.name);
    });

    for (const auto& m : matches) {
        if (show_sha) {
            std::cout << "Package: " << m.name << m.label << "\n";
            std::cout << hash_label << ": " << m.sha << "\n";
        } else {
            std::cout << m.name << m.label << "\n";
        }
    }
    if (matches.empty()) std::cerr << "No matches found.\n";

    if (!age_paths.empty()) {
        time_t newest = 0;
        for (const auto& p : age_paths) {
            struct stat st;
            if (stat(p.c_str(), &st) == 0 && st.st_mtime > newest) newest = st.st_mtime;
        }
        if (newest > 0) {
            double days = difftime(time(nullptr), newest) / 86400.0;
            if (days > 7)
                std::cerr << "Note: the list is " << (int)days
                          << " days old, run with --update for newer ones\n";
        }
    }
}

// ---------------------------------------------------------------------------
// Argument parsing
// ---------------------------------------------------------------------------

struct Args {
    std::vector<std::string> queries;
    std::string os;
    bool sha256 = false;
    bool sha512 = false;
    bool update = false;
    std::string release;
    std::string arch;
    bool list_releases = false;
    bool exact = false;
    bool error = false;
};

static bool valid_release(const std::string& v) {
    static const std::regex re(R"(\d+\.\d+(_[a-z0-9q]+)?|snapshots)");
    return std::regex_match(v, re);
}

static bool valid_arch(const std::string& v) {
    static const std::regex re(R"([a-z0-9]+)");
    return std::regex_match(v, re);
}

static void print_help(const std::string& prog) {
    std::cout <<
        "usage: " << prog << " [-h] [--os {openbsd,netbsd}] [--sha256] [--sha512]\n"
        "                     [--update] [--release RELEASE] [--arch ARCH]\n"
        "                     [--list-releases] [-e]\n"
        "                     [package ...]\n\n"
        "Search OpenBSD and NetBSD package lists from a local database.\n\n"
        "positional arguments:\n"
        "  package               words to search for, case-insensitive; a\n"
        "                        package must contain all of them. A word with\n"
        "                        any of ^$*?()[]{}|\\ is a regular expression\n\n"
        "options:\n"
        "  -h, --help            show this help message and exit\n"
        "  --os {openbsd,netbsd} which package universe to search (default:\n"
        "                        auto-detect)\n"
        "  --sha256              also show each package's SHA256 sum (OpenBSD)\n"
        "  --sha512              also show each package's SHA512 sum (NetBSD)\n"
        "  --update              re-download the package lists\n"
        "  --release RELEASE     release to search, downloaded if needed\n"
        "  --arch ARCH           package architecture, downloaded if needed\n"
        "  --list-releases       list available releases on the mirror\n"
        "  -e, --exact           match whole package names and show newest\n\n"
        "Package lists are saved under " << g_cache_root << ".\n";
}

static Args parse_args(int argc, char** argv, const std::string& prog) {
    Args a;
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        auto need_value = [&](const char* name) -> std::string {
            if (i + 1 >= argc) {
                std::cerr << "error: " << name << " requires a value\n";
                a.error = true;
                return "";
            }
            return argv[++i];
        };
        if (arg == "-h" || arg == "--help") {
            print_help(prog);
            std::exit(0);
        } else if (arg == "--os") {
            a.os = to_lower(need_value("--os"));
            if (a.os != "openbsd" && a.os != "netbsd") {
                std::cerr << "error: --os must be openbsd or netbsd\n";
                a.error = true;
            }
        } else if (arg == "--sha256") {
            a.sha256 = true;
        } else if (arg == "--sha512") {
            a.sha512 = true;
        } else if (arg == "--update") {
            a.update = true;
        } else if (arg == "--release") {
            a.release = need_value("--release");
            if (!valid_release(a.release)) {
                std::cerr << "error: release '" << a.release
                          << "' is not like 7.8, snapshots or 11.0_2026Q3\n";
                a.error = true;
            }
        } else if (arg == "--arch") {
            a.arch = need_value("--arch");
            if (!valid_arch(a.arch)) {
                std::cerr << "error: arch '" << a.arch << "' is not like amd64\n";
                a.error = true;
            }
        } else if (arg == "--list-releases") {
            a.list_releases = true;
        } else if (arg == "-e" || arg == "--exact") {
            a.exact = true;
        } else if (!arg.empty() && arg[0] == '-' && arg != "-") {
            std::cerr << "error: unrecognized argument: " << arg << "\n";
            a.error = true;
        } else {
            a.queries.push_back(arg);
        }
    }
    return a;
}

// ---------------------------------------------------------------------------
// Main
// ---------------------------------------------------------------------------

static int run_openbsd(const Args& args) {
    std::string mirror = openbsd_mirror();
    std::string release = args.release.empty() ? openbsd_default_release(mirror) : args.release;
    std::string arch = args.arch.empty() ? openbsd_default_arch() : args.arch;

    if (args.list_releases) {
        auto releases = openbsd_mirror_releases(mirror);
        for (const auto& r : releases) std::cout << r << "\n";
        return 0;
    }

    std::vector<std::string> missing;
    for (const auto& folder : OPENBSD_FOLDERS) {
        if (args.update || !fs::is_regular_file(local_list("openbsd", release, arch, folder)))
            missing.push_back(folder);
    }
    for (const auto& folder : missing) {
        if (!openbsd_download(mirror, release, arch, folder)) return 1;
    }

    if (!args.queries.empty()) {
        std::vector<std::regex> patterns;
        for (const auto& w : args.queries) patterns.push_back(word_pattern(w));
        std::vector<std::pair<std::string,
            std::vector<std::pair<std::string, std::string>>>> sources;
        for (const auto& folder : OPENBSD_FOLDERS) {
            std::string label = (folder == "packages-stable") ? " (stable)" : "";
            std::string db = local_list("openbsd", release, arch, folder);
            sources.emplace_back(label, openbsd_parse(db));
        }
        search(sources, patterns, args.queries, args.sha256, args.exact,
               {local_list("openbsd", release, arch, "packages-stable")},
               "SHA256", false);
    } else if (missing.empty()) {
        std::cerr << "ERROR: Please supply some package!\n";
        return 1;
    }
    return 0;
}

static int run_netbsd(const Args& args) {
    std::string mirror = netbsd_mirror();
    std::string arch = args.arch.empty() ? netbsd_default_arch() : args.arch;

    if (args.list_releases) {
        auto releases = netbsd_mirror_releases(mirror, arch);
        for (const auto& r : releases) std::cout << r << "\n";
        return 0;
    }

    std::string release = args.release.empty() ? netbsd_default_release(mirror, arch) : args.release;
    std::string db = local_list("netbsd", release, arch, "packages", "SHA512");

    if (args.update || !fs::is_regular_file(db)) {
        if (!netbsd_download(mirror, release, arch)) return 1;
    }

    if (!args.queries.empty()) {
        std::vector<std::regex> patterns;
        for (const auto& w : args.queries) patterns.push_back(word_pattern(w));
        std::vector<std::pair<std::string,
            std::vector<std::pair<std::string, std::string>>>> sources;
        sources.emplace_back("", netbsd_parse(db));
        search(sources, patterns, args.queries, args.sha512, args.exact,
               {db}, "SHA512", true);
    } else if (!args.update) {
        std::cerr << "ERROR: Please supply some package!\n";
        return 1;
    }
    return 0;
}

int main(int argc, char *argv[]) {
    std::string prog = fs::path(argv[0]).filename().string();
    const char* home = std::getenv("HOME");
    g_cache_root = (fs::path(home ? home : ".") / ".cache" / "0verbs").string();

    curl_global_init(CURL_GLOBAL_DEFAULT);

    Args args = parse_args(argc, argv, prog);
    if (args.error) {
        curl_global_cleanup();
        return EXIT_FAILURE;
    }

    std::string target = args.os.empty() ? detect_os() : args.os;
    int rc = (target == "openbsd") ? run_openbsd(args) : run_netbsd(args);

    curl_global_cleanup();
    return rc;
}