/* t3k.cpp -- Tone3000 client inside nam_vst.so (see t3k.h).
 *
 * libcurl is dlopen'd (the device's libcurl.so.4), so the plugin still loads -- with the TONE3000
 * tab reporting "no network library" -- on a system without it, and the build needs no curl
 * headers. Option numbers are curl's stable ABI values (curl/curl.h).
 *
 * Sign-in: the tab shows http://<lan-ip>:8090. That page links to /login, which redirects the
 * phone to Tone3000's OAuth consent with a PKCE challenge; Tone3000 sends it back to /callback on
 * the same host:port the phone used, where the code is exchanged for tokens. Tokens persist in
 * /storage/nam-tone3000/tokens.json and are refreshed (rotating refresh token) before API calls.
 *
 * Threads: one worker (API calls, downloads) and one tiny HTTP server, both process-wide and
 * refcounted by start()/stop(). Every blocking call has a timeout or checks g_run, so stop() --
 * called from effClose on MPC's UI thread -- returns within ~a second.
 */
#include "t3k.h"
#include "json.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <deque>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <sstream>
#include <thread>

#include <arpa/inet.h>
#include <dlfcn.h>
#include <fcntl.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

namespace t3k {
namespace {

using json = nlohmann::json;
namespace fs = std::filesystem;

constexpr const char *API = "https://www.tone3000.com/api/v1";
constexpr const char *CONF_DIR = "/storage/nam-tone3000";
/* Publishable OAuth client id of this app's Tone3000 registration. Public by design (PKCE public
 * client, no client secret); {"client_id": ...} in <CONF_DIR>/config.json overrides it. */
constexpr const char *DEFAULT_CLIENT_ID = "t3k_pub_Yr1pHJdrZ65EglSR4mIf70WJ5wcazzjE";
constexpr int LOGIN_PORT = 8090;

/* ---------------- libcurl, loaded at runtime ---------------- */
enum {
    OPT_WRITEDATA = 10001, OPT_URL = 10002, OPT_WRITEFUNCTION = 20011, OPT_TIMEOUT = 13,
    OPT_LOW_SPEED_LIMIT = 19, OPT_LOW_SPEED_TIME = 20, OPT_POSTFIELDS = 10015, OPT_USERAGENT = 10018,
    OPT_HTTPHEADER = 10023, OPT_NOPROGRESS = 43, OPT_POST = 47, OPT_FOLLOWLOCATION = 52,
    OPT_POSTFIELDSIZE = 60, OPT_SSL_VERIFYPEER = 64, OPT_CAINFO = 10065, OPT_CONNECTTIMEOUT = 78,
    OPT_HTTPGET = 80, OPT_SSL_VERIFYHOST = 81, OPT_XFERINFOFUNCTION = 20219,
    INFO_RESPONSE_CODE = 0x200002,
};
struct curl_slist { char *data; curl_slist *next; };
struct Curl {
    void *h = nullptr;
    void *(*easy_init)() = nullptr;
    int (*easy_setopt)(void *, int, ...) = nullptr;
    int (*easy_perform)(void *) = nullptr;
    int (*easy_getinfo)(void *, int, ...) = nullptr;
    void (*easy_cleanup)(void *) = nullptr;
    curl_slist *(*slist_append)(curl_slist *, const char *) = nullptr;
    void (*slist_free_all)(curl_slist *) = nullptr;
    int (*global_init)(long) = nullptr;
    bool ok() const { return easy_init && easy_setopt && easy_perform && easy_getinfo && easy_cleanup && slist_append && slist_free_all; }
} C;
bool load_curl() {
    if (C.ok()) return true;
    for (const char *name : {"libcurl.so.4", "libcurl.so", "libcurl-gnutls.so.4"})
        if ((C.h = dlopen(name, RTLD_NOW | RTLD_LOCAL))) break;
    if (!C.h) return false;
    C.easy_init = (void *(*)())dlsym(C.h, "curl_easy_init");
    C.easy_setopt = (int (*)(void *, int, ...))dlsym(C.h, "curl_easy_setopt");
    C.easy_perform = (int (*)(void *))dlsym(C.h, "curl_easy_perform");
    C.easy_getinfo = (int (*)(void *, int, ...))dlsym(C.h, "curl_easy_getinfo");
    C.easy_cleanup = (void (*)(void *))dlsym(C.h, "curl_easy_cleanup");
    C.slist_append = (curl_slist * (*)(curl_slist *, const char *)) dlsym(C.h, "curl_slist_append");
    C.slist_free_all = (void (*)(curl_slist *))dlsym(C.h, "curl_slist_free_all");
    C.global_init = (int (*)(long))dlsym(C.h, "curl_global_init");
    if (C.global_init) C.global_init(3 /* CURL_GLOBAL_ALL */);
    return C.ok();
}

std::atomic<bool> g_run{false};

size_t write_string(char *p, size_t sz, size_t n, void *ud) { ((std::string *)ud)->append(p, sz * n); return sz * n; }
size_t write_file(char *p, size_t sz, size_t n, void *ud) { return fwrite(p, 1, sz * n, (FILE *)ud); }
int abort_on_stop(void *, long long, long long, long long, long long) { return g_run.load() ? 0 : 1; }

/* GET (body empty) or form POST. Returns the HTTP status, or -1 on a transport failure. When `to`
 * is set the body streams into that file instead of `out`. */
long http(const std::string &url, const std::string *post_body, const std::string &bearer, std::string *out, FILE *to = nullptr,
          long timeout_s = 20) {
    void *c = C.easy_init();
    if (!c) return -1;
    curl_slist *hdr = nullptr;
    std::string auth;
    if (!bearer.empty()) { auth = "Authorization: Bearer " + bearer; hdr = C.slist_append(hdr, auth.c_str()); }
    C.easy_setopt(c, OPT_URL, url.c_str());
    if (to) { C.easy_setopt(c, OPT_WRITEFUNCTION, write_file); C.easy_setopt(c, OPT_WRITEDATA, to); }
    else { C.easy_setopt(c, OPT_WRITEFUNCTION, write_string); C.easy_setopt(c, OPT_WRITEDATA, out); }
    C.easy_setopt(c, OPT_USERAGENT, "nam-vst-t3k/1.0");
    C.easy_setopt(c, OPT_FOLLOWLOCATION, 1L);
    C.easy_setopt(c, OPT_CONNECTTIMEOUT, 10L);
    C.easy_setopt(c, OPT_TIMEOUT, timeout_s);
    C.easy_setopt(c, OPT_LOW_SPEED_LIMIT, 256L);   /* stalled for 20s -> give up instead of waiting out the timeout */
    C.easy_setopt(c, OPT_LOW_SPEED_TIME, 20L);
    C.easy_setopt(c, OPT_NOPROGRESS, 0L);
    C.easy_setopt(c, OPT_XFERINFOFUNCTION, abort_on_stop);
    C.easy_setopt(c, OPT_SSL_VERIFYPEER, 1L);
    C.easy_setopt(c, OPT_SSL_VERIFYHOST, 2L);
    C.easy_setopt(c, OPT_CAINFO, "/etc/ssl/certs/ca-certificates.crt");
    if (hdr) C.easy_setopt(c, OPT_HTTPHEADER, hdr);
    if (post_body) {
        C.easy_setopt(c, OPT_POST, 1L);
        C.easy_setopt(c, OPT_POSTFIELDS, post_body->c_str());
        C.easy_setopt(c, OPT_POSTFIELDSIZE, (long)post_body->size());
    } else C.easy_setopt(c, OPT_HTTPGET, 1L);
    long code = -1;
    if (C.easy_perform(c) == 0) C.easy_getinfo(c, INFO_RESPONSE_CODE, &code);
    C.easy_cleanup(c);
    if (hdr) C.slist_free_all(hdr);
    return code;
}

std::string urlenc(const std::string &s) {
    static const char *hex = "0123456789ABCDEF";
    std::string o;
    for (unsigned char ch : s) {
        if (isalnum(ch) || ch == '-' || ch == '_' || ch == '.' || ch == '~') o += (char)ch;
        else { o += '%'; o += hex[ch >> 4]; o += hex[ch & 15]; }
    }
    return o;
}
std::string urldec(const std::string &s) {
    std::string o;
    for (size_t i = 0; i < s.size(); i++) {
        if (s[i] == '%' && i + 2 < s.size() && isxdigit((unsigned char)s[i + 1]) && isxdigit((unsigned char)s[i + 2])) {
            o += (char)std::stoi(s.substr(i + 1, 2), nullptr, 16); i += 2;
        } else o += s[i] == '+' ? ' ' : s[i];
    }
    return o;
}
std::string html_esc(const std::string &s) {
    std::string o;
    for (char ch : s) o += ch == '<' ? "&lt;" : ch == '>' ? "&gt;" : ch == '&' ? "&amp;" : ch == '"' ? "&quot;" : std::string(1, ch);
    return o;
}
/* Percent-encode a query-parameter value (make names carry spaces, e.g. "Mesa Boogie"). */
std::string url_enc(const std::string &s) {
    static const char *hex = "0123456789ABCDEF";
    std::string o;
    for (unsigned char c : s) {
        if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') o += (char)c;
        else { o += '%'; o += hex[c >> 4]; o += hex[c & 15]; }
    }
    return o;
}
std::string read_file(const std::string &p) {
    std::ifstream f(p, std::ios::binary);
    std::ostringstream ss;
    if (f) ss << f.rdbuf();
    return ss.str();
}
void write_file_atomic(const std::string &p, const std::string &data) {
    std::string tmp = p + ".tmp";
    { std::ofstream f(tmp, std::ios::binary | std::ios::trunc); if (!f) return; f << data; }
    std::rename(tmp.c_str(), p.c_str());
}

/* ---------------- PKCE: SHA-256 + base64url ---------------- */
struct Sha256 {
    uint32_t st[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
    unsigned char buf[64]; size_t n = 0; uint64_t bits = 0;
    static uint32_t rotr(uint32_t x, int k) { return (x >> k) | (x << (32 - k)); }
    void block(const unsigned char *d) {
        static const uint32_t K[64] = {
            0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5,
            0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,
            0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,
            0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,0x06ca6351,0x14292967,
            0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,
            0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,0xf40e3585,0x106aa070,
            0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3,
            0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2};
        uint32_t m[64];
        for (int i = 0; i < 16; i++) m[i] = (uint32_t)d[4 * i] << 24 | (uint32_t)d[4 * i + 1] << 16 | (uint32_t)d[4 * i + 2] << 8 | d[4 * i + 3];
        for (int i = 16; i < 64; i++)
            m[i] = m[i - 16] + (rotr(m[i - 15], 7) ^ rotr(m[i - 15], 18) ^ (m[i - 15] >> 3)) + m[i - 7] +
                   (rotr(m[i - 2], 17) ^ rotr(m[i - 2], 19) ^ (m[i - 2] >> 10));
        uint32_t a = st[0], b = st[1], c = st[2], dd = st[3], e = st[4], f = st[5], g = st[6], h = st[7];
        for (int i = 0; i < 64; i++) {
            uint32_t t1 = h + (rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25)) + ((e & f) ^ (~e & g)) + K[i] + m[i];
            uint32_t t2 = (rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22)) + ((a & b) ^ (a & c) ^ (b & c));
            h = g; g = f; f = e; e = dd + t1; dd = c; c = b; b = a; a = t1 + t2;
        }
        st[0] += a; st[1] += b; st[2] += c; st[3] += dd; st[4] += e; st[5] += f; st[6] += g; st[7] += h;
    }
    void update(const std::string &s) {
        for (unsigned char ch : s) { buf[n++] = ch; bits += 8; if (n == 64) { block(buf); n = 0; } }
    }
    std::string digest() {
        uint64_t total = bits;
        buf[n++] = 0x80;
        if (n > 56) { while (n < 64) buf[n++] = 0; block(buf); n = 0; }
        while (n < 56) buf[n++] = 0;
        for (int i = 7; i >= 0; i--) buf[n++] = (unsigned char)(total >> (i * 8));
        block(buf);
        std::string out;
        for (uint32_t w : st) for (int i = 3; i >= 0; i--) out += (char)(w >> (i * 8));
        return out;
    }
};
std::string b64url(const std::string &d) {
    static const char *t = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
    std::string o;
    for (size_t i = 0; i < d.size(); i += 3) {
        uint32_t v = (uint32_t)(unsigned char)d[i] << 16;
        if (i + 1 < d.size()) v |= (uint32_t)(unsigned char)d[i + 1] << 8;
        if (i + 2 < d.size()) v |= (unsigned char)d[i + 2];
        o += t[v >> 18 & 63]; o += t[v >> 12 & 63];
        if (i + 1 < d.size()) o += t[v >> 6 & 63];
        if (i + 2 < d.size()) o += t[v & 63];
    }
    return o;
}
std::string random_b64(size_t n) {
    std::string raw(n, '\0');
    int fd = open("/dev/urandom", O_RDONLY);
    if (fd >= 0) { if (read(fd, raw.data(), n) != (ssize_t)n) {} close(fd); }
    return b64url(raw);
}

/* ---------------- shared state ---------------- */
struct Job { long long seq; const void *owner; bool download; int sort, page, page_size; long long tone_id; std::string name, dir; Filters filt; };

std::mutex g_mtx;               /* guards everything below */
std::condition_variable g_cv;
int g_refs = 0;
std::thread g_worker, g_server;
std::deque<Job> g_jobs;
long long g_last_seq = 0;
Status g_status;
std::map<long long, Results> g_results;   /* last few answers, keyed by request seq */
std::string g_client_id, g_access, g_refresh;
long long g_expires_at = 0;
std::string g_pkce_verifier, g_pkce_state, g_pkce_redirect;
long long g_pkce_created = 0;

std::string tokens_path() { return std::string(CONF_DIR) + "/tokens.json"; }

std::string lan_ip() {
    std::string ip = "127.0.0.1";
    ifaddrs *ifs = nullptr;
    if (getifaddrs(&ifs) != 0) return ip;
    for (ifaddrs *i = ifs; i; i = i->ifa_next)
        if (i->ifa_addr && i->ifa_addr->sa_family == AF_INET && (i->ifa_flags & IFF_UP) && !(i->ifa_flags & IFF_LOOPBACK)) {
            char b[INET_ADDRSTRLEN];
            inet_ntop(AF_INET, &((sockaddr_in *)i->ifa_addr)->sin_addr, b, sizeof b);
            ip = b;
            break;
        }
    freeifaddrs(ifs);
    return ip;
}

void load_config_locked() {
    mkdir(CONF_DIR, 0755);
    g_client_id = DEFAULT_CLIENT_ID;
    try {
        std::string id = json::parse(read_file(std::string(CONF_DIR) + "/config.json")).value("client_id", std::string());
        if (!id.empty()) g_client_id = id;
    } catch (...) {}
    try {
        auto j = json::parse(read_file(tokens_path()));
        g_access = j.value("access_token", std::string());
        g_refresh = j.value("refresh_token", std::string());
        g_expires_at = j.value("expires_at", 0LL);
    } catch (...) {}
    g_status.signed_in = !g_access.empty() || !g_refresh.empty();
    g_status.login_url = "http://" + lan_ip() + ":" + std::to_string(LOGIN_PORT);
}
void save_tokens_locked() {
    write_file_atomic(tokens_path(), json({{"access_token", g_access}, {"refresh_token", g_refresh}, {"expires_at", g_expires_at}}).dump() + "\n");
}
/* POST /oauth/token and adopt the answer. Called without g_mtx held (network). Returns 200 on
 * success, else the HTTP status (-1 = transport failure, -2 = unusable reply). */
long token_request(const std::string &body) {
    std::string resp;
    long code = http(std::string(API) + "/oauth/token", &body, "", &resp);
    if (code != 200) return code;
    try {
        auto j = json::parse(resp);
        std::string at = j.value("access_token", std::string());
        if (at.empty()) return -2;
        std::lock_guard<std::mutex> lk(g_mtx);
        g_access = at;
        std::string rt = j.value("refresh_token", std::string());
        if (!rt.empty()) g_refresh = rt;
        g_expires_at = (long long)time(nullptr) + j.value("expires_in", 3600LL) - 30;
        g_status.signed_in = true;
        save_tokens_locked();
        return 200;
    } catch (...) { return -2; }
}
/* A usable access token, refreshing it first if it is about to expire. "" with *err = 0 means
 * signed out; with *err != 0 the refresh couldn't reach Tone3000 and the login is kept for later. */
std::string access_token(long *err) {
    *err = 0;
    std::string rt, cid;
    {
        std::lock_guard<std::mutex> lk(g_mtx);
        if (!g_access.empty() && g_expires_at - (long long)time(nullptr) > 30) return g_access;
        rt = g_refresh; cid = g_client_id;
    }
    if (rt.empty()) { std::lock_guard<std::mutex> lk(g_mtx); g_status.signed_in = false; return std::string(); }
    long code = token_request("grant_type=refresh_token&refresh_token=" + urlenc(rt) + "&client_id=" + urlenc(cid));
    std::lock_guard<std::mutex> lk(g_mtx);
    if (code == 200) return g_access;
    if (code == 400 || code == 401) {   /* refresh token rejected: a real sign-out */
        if (rt == g_refresh) { g_access.clear(); g_refresh.clear(); g_expires_at = 0; save_tokens_locked(); }
        g_status.signed_in = false;
        return std::string();
    }
    *err = code;
    return std::string();
}

void set_status(long long seq, const std::string &state, const std::string &msg) {
    std::lock_guard<std::mutex> lk(g_mtx);
    g_status.seq = seq; g_status.state = state; g_status.message = msg;
}
std::string http_error(long code) {
    if (code == 401) return "sign in again";
    if (code == 429) return "rate limited, wait a minute";
    if (code <= 0) return "no network";
    return "HTTP " + std::to_string(code);
}

/* File-name-safe: letters, digits, space, - _ ( ) . & ' kept; everything else dropped. */
std::string safe_name(const std::string &in, bool spaces_to_underscore) {
    std::string o;
    for (unsigned char ch : in)
        if (isalnum(ch) || ch == '-' || ch == '_' || ch == '(' || ch == ')' || ch == '&' || ch == '\'' || ch == ' ' || (ch == '.' && !o.empty()))
            if (!(ch == ' ' && (o.empty() || o.back() == ' ' || o.back() == '_'))) o += (spaces_to_underscore && ch == ' ') ? '_' : (char)ch;
    while (!o.empty() && (o.back() == ' ' || o.back() == '.')) o.pop_back();
    if (o.size() > 80) o.resize(80);
    return o.empty() ? std::string("model") : o;
}

void do_search(const Job &jb) {
    set_status(jb.seq, "searching", "");
    long terr;
    std::string tok = access_token(&terr);
    if (tok.empty()) { set_status(jb.seq, terr ? "error" : "idle", terr ? http_error(terr) : ""); return; }   /* idle + signed out: status shows the login URL */
    std::string url = std::string(API) + (jb.sort == 3 ? "/tones/favorited?" : "/tones/search?format=nam&sort=" +
                      std::string(jb.sort == 1 ? "newest" : jb.sort == 2 ? "downloads-all-time" : "trending") + "&") +
                      "page=" + std::to_string(jb.page < 1 ? 1 : jb.page) + "&page_size=" + std::to_string(jb.page_size);
    /* Filters apply to /tones/search only; /tones/favorited takes none. */
    if (jb.sort != 3) {
        const Filters &f = jb.filt;
        if (!f.gears.empty())      url += "&gears=" + f.gears;
        if (!f.sizes.empty())      url += "&sizes=" + f.sizes;
        if (f.architecture)        url += "&architecture=" + std::to_string(f.architecture);
        if (f.calibrated)          url += "&calibrated=true";
        if (f.verified)            url += "&verified=true";
        if (!f.make.empty())       url += "&makes=" + url_enc(f.make);
    }
    std::string resp;
    long code = http(url, nullptr, tok, &resp);
    Results r;
    r.seq = jb.seq;
    if (code == 200) {
        try {
            auto j = json::parse(resp);
            r.total_pages = std::max(1, j.value("total_pages", 1));
            if (j.contains("data") && j["data"].is_array())
                for (auto &it : j["data"]) {
                    if ((int)r.items.size() >= jb.page_size) break;
                    r.items.push_back({it.value("id", 0LL), it.value("title", std::string())});
                }
        } catch (...) { code = -2; }
    }
    {
        std::lock_guard<std::mutex> lk(g_mtx);
        g_results[jb.seq] = r;
        while (g_results.size() > 16) g_results.erase(g_results.begin());
    }
    if (code == 401) { std::lock_guard<std::mutex> lk(g_mtx); g_access.clear(); g_expires_at = 0; }
    set_status(jb.seq, code == 200 ? "idle" : "error", code == 200 ? "" : code == -2 ? "bad reply from Tone3000" : http_error(code));
}

/* sizes is the hyphen-joined list sent to /tones/search, e.g. "lite-feather-nano". */
bool size_listed(const std::string &sizes, const std::string &size) {
    if (size.empty()) return false;
    size_t p = 0;
    while (p <= sizes.size()) {
        size_t e = sizes.find('-', p);
        if (e == std::string::npos) e = sizes.size();
        if (sizes.compare(p, e - p, size) == 0) return true;
        p = e + 1;
    }
    return false;
}

void do_download(const Job &jb) {
    set_status(jb.seq, "downloading", "");
    long terr;
    std::string tok = access_token(&terr);
    if (tok.empty()) { set_status(jb.seq, "error", terr ? http_error(terr) : "sign in first"); return; }
    /* /models has no sizes parameter and, without architecture, returns A1 + custom only, so a tone
     * found under the A2 filters would otherwise download its heavy A1 captures. */
    std::string url = std::string(API) + "/models?tone_id=" + std::to_string(jb.tone_id) + "&page_size=300";
    if (jb.filt.architecture) url += "&architecture=" + std::to_string(jb.filt.architecture);
    std::string resp;
    long code = http(url, nullptr, tok, &resp);
    if (code != 200) { set_status(jb.seq, "error", "fetch models: " + http_error(code)); return; }
    json all, data = json::array();
    try { all = json::parse(resp).value("data", json::array()); } catch (...) {}
    if (!all.is_array() || all.empty()) { set_status(jb.seq, "error", "no captures in this tone"); return; }
    for (auto &m : all) {
        std::string size = m.contains("size") && m["size"].is_string() ? m["size"].get<std::string>() : "";
        if (jb.filt.sizes.empty() || size_listed(jb.filt.sizes, size)) data.push_back(m);
    }
    if (data.empty()) { set_status(jb.seq, "error", "no captures fit ARCH"); return; }
    /* A pack gets its own folder (BROWSE shows it as one row); a single capture lands loose. */
    std::string pack = data.size() > 1 ? safe_name(jb.name, false) : std::string();
    fs::path dest = pack.empty() ? fs::path(jb.dir) : fs::path(jb.dir) / pack;
    std::error_code ec;
    fs::create_directories(dest, ec);
    int saved = 0, total = (int)data.size();
    std::string last;
    for (int i = 0; i < total && g_run.load(); i++) {
        set_status(jb.seq, "downloading", std::to_string(i + 1) + "/" + std::to_string(total));
        auto &m = data[(size_t)i];
        std::string murl = m.value("model_url", std::string());
        if (murl.empty()) continue;
        std::string base = safe_name(m.value("name", std::string("model")), true);
        fs::path final_path = dest / (base + ".nam");
        /* Written under a hidden name, renamed when complete: BROWSE rescans while this runs and
         * must never list (or load) a half-written capture. */
        fs::path part = dest / ("." + base + ".part");
        FILE *f = fopen(part.c_str(), "wb");
        if (!f) continue;
        long c2 = http(murl, nullptr, tok, nullptr, f, 120);
        fclose(f);
        if (c2 == 200) { fs::rename(part, final_path, ec); if (!ec) { saved++; last = base; } }
        else fs::remove(part, ec);
    }
    if (!g_run.load()) return;
    if (saved == 0) { if (!pack.empty()) fs::remove(dest, ec); set_status(jb.seq, "error", "download failed"); return; }
    std::lock_guard<std::mutex> lk(g_mtx);
    g_status.seq = jb.seq; g_status.state = "idle"; g_status.message = std::to_string(saved);
    g_status.last_download = pack.empty() ? last : pack;
}

void worker_main() {
    while (true) {
        Job jb;
        {
            std::unique_lock<std::mutex> lk(g_mtx);
            g_cv.wait(lk, [] { return !g_run.load() || !g_jobs.empty(); });
            if (!g_run.load()) return;
            jb = g_jobs.front();
            g_jobs.pop_front();
        }
        if (!load_curl()) { set_status(jb.seq, "error", "libcurl not found"); continue; }
        if (jb.download) do_download(jb);
        else do_search(jb);
    }
}

/* ---------------- sign-in web page (LAN :8090) ---------------- */
void send_all(int fd, const std::string &s) {
    size_t off = 0;
    while (off < s.size()) { ssize_t w = send(fd, s.data() + off, s.size() - off, MSG_NOSIGNAL); if (w <= 0) return; off += (size_t)w; }
}
void reply(int fd, const std::string &status, const std::string &body, const std::string &extra = "") {
    send_all(fd, "HTTP/1.1 " + status + "\r\nContent-Type: text/html; charset=utf-8\r\nContent-Length: " + std::to_string(body.size()) +
                 "\r\nCache-Control: no-store\r\nConnection: close\r\n" + extra + "\r\n" + body);
}
/* Same synthwave palette as the plugin skin (layout.conf theme_*). */
std::string page(const std::string &inner) {
    return "<!doctype html><html lang=en><head><meta charset=utf-8>"
           "<meta name=viewport content=\"width=device-width,initial-scale=1\"><title>NAM x Tone3000</title><style>"
           ":root{--bg:#15101f;--panel:#1e1533;--line:#46306a;--ink:#f4ecff;--dim:#9a86c8;--pink:#ff2e88;--cyan:#24e0ff}"
           "*{box-sizing:border-box}html,body{margin:0;min-height:100%;background:var(--bg);color:var(--ink);"
           "font:16px/1.5 system-ui,-apple-system,Segoe UI,sans-serif}"
           "main{max-width:420px;margin:0 auto;padding:48px 20px}"
           ".eyebrow{font-size:12px;letter-spacing:.18em;text-transform:uppercase;color:var(--cyan);margin:0 0 8px}"
           "h1{font-size:28px;line-height:1.15;margin:0 0 16px}p{color:var(--dim);margin:0 0 16px}"
           ".card{background:var(--panel);border:1px solid var(--line);border-radius:14px;padding:20px;margin:24px 0}"
           ".row{display:flex;justify-content:space-between;gap:12px;padding:6px 0}.row span:first-child{color:var(--dim)}"
           ".ok{color:var(--cyan)}.btn{display:block;text-align:center;background:var(--pink);color:#15101f;font-weight:700;"
           "text-decoration:none;padding:14px;border-radius:10px;font-size:17px;margin-top:8px}"
           ".btn:active{transform:translateY(1px)}a.quiet{color:var(--dim);font-size:14px}"
           "</style></head><body><main>" + inner + "</main></body></html>";
}
void serve_home(int fd) {
    Status st = status();
    std::string h = "<p class=eyebrow>Neural Amp Modeler</p><h1>Tone3000 on your MPC</h1>";
    if (st.signed_in) {
        h += "<div class=card><div class=row><span>Account</span><span class=ok>Signed in</span></div>";
        if (!st.last_download.empty()) h += "<div class=row><span>Last download</span><span>" + html_esc(st.last_download) + "</span></div>";
        h += "</div><p>Browse and download from the plugin's TONE3000 tab. Captures land in BROWSE right away.</p>"
             "<a class=quiet href=/signout>Sign out</a>";
    } else {
        h += "<p>Sign in once and the plugin can search and download captures straight onto the MPC.</p>"
             "<a class=btn href=/login>Sign in with Tone3000</a>";
    }
    reply(fd, "200 OK", page(h));
}
void serve_login(int fd, const std::string &host) {
    std::string url;
    {
        std::lock_guard<std::mutex> lk(g_mtx);
        load_config_locked();   /* pick up a config.json override edited since the plugin loaded */
        g_pkce_verifier = random_b64(32);
        g_pkce_state = random_b64(16);
        g_pkce_created = (long long)time(nullptr);
        /* Come back to whatever address the phone reached us on -- it's the one that works. */
        g_pkce_redirect = "http://" + (host.empty() ? lan_ip() + ":" + std::to_string(LOGIN_PORT) : host) + "/callback";
        Sha256 sh; sh.update(g_pkce_verifier);
        url = std::string(API) + "/oauth/authorize?client_id=" + urlenc(g_client_id) + "&redirect_uri=" + urlenc(g_pkce_redirect) +
              "&response_type=code&code_challenge=" + urlenc(b64url(sh.digest())) + "&code_challenge_method=S256&state=" +
              urlenc(g_pkce_state);
    }
    reply(fd, "302 Found", "", "Location: " + url + "\r\n");
}
std::string qget(const std::string &q, const std::string &key) {
    size_t pos = 0;
    while (pos <= q.size()) {
        size_t amp = q.find('&', pos);
        std::string kv = q.substr(pos, amp == std::string::npos ? std::string::npos : amp - pos);
        if (kv.compare(0, key.size() + 1, key + "=") == 0) return urldec(kv.substr(key.size() + 1));
        if (amp == std::string::npos) break;
        pos = amp + 1;
    }
    return std::string();
}
void serve_callback(int fd, const std::string &q) {
    std::string code = qget(q, "code"), state = qget(q, "state"), verifier, redirect, cid;
    bool ok;
    {
        std::lock_guard<std::mutex> lk(g_mtx);
        ok = !code.empty() && !g_pkce_state.empty() && state == g_pkce_state && time(nullptr) - g_pkce_created < 600;
        verifier = g_pkce_verifier; redirect = g_pkce_redirect; cid = g_client_id;
        if (ok) g_pkce_state.clear();
    }
    std::string err = qget(q, "error_description").empty() ? qget(q, "error") : qget(q, "error_description");
    if (ok && load_curl() &&
        200 == token_request("grant_type=authorization_code&code=" + urlenc(code) + "&code_verifier=" + urlenc(verifier) +
                      "&redirect_uri=" + urlenc(redirect) + "&client_id=" + urlenc(cid))) {
        reply(fd, "200 OK", page("<p class=eyebrow>Neural Amp Modeler</p><h1>You're signed in</h1>"
                                 "<p>Head back to the MPC -- the TONE3000 tab is ready. You can close this tab.</p>"));
        return;
    }
    reply(fd, "200 OK", page("<p class=eyebrow>Neural Amp Modeler</p><h1>Sign-in didn't finish</h1><p>" +
                             html_esc(!err.empty() ? err : ok ? "Tone3000 didn't accept the sign-in code." :
                                                            "This sign-in link expired or was already used.") +
                             "</p><a class=btn href=/login>Try again</a>"));
}
void serve_signout(int fd) {
    {
        std::lock_guard<std::mutex> lk(g_mtx);
        g_access.clear(); g_refresh.clear(); g_expires_at = 0; g_status.signed_in = false;
        save_tokens_locked();
    }
    reply(fd, "302 Found", "", "Location: /\r\n");
}
void handle(int fd) {
    std::string req;
    char buf[2048];
    pollfd p{fd, POLLIN, 0};
    while (req.find("\r\n\r\n") == std::string::npos && req.size() < 8192 && poll(&p, 1, 3000) > 0) {
        ssize_t r = recv(fd, buf, sizeof buf, 0);
        if (r <= 0) break;
        req.append(buf, (size_t)r);
    }
    size_t sp1 = req.find(' '), sp2 = sp1 == std::string::npos ? sp1 : req.find(' ', sp1 + 1);
    if (sp2 == std::string::npos) return;
    std::string target = req.substr(sp1 + 1, sp2 - sp1 - 1), path = target, q;
    if (size_t qm = target.find('?'); qm != std::string::npos) { path = target.substr(0, qm); q = target.substr(qm + 1); }
    std::string host;
    for (const char *key : {"\r\nHost: ", "\r\nhost: "})
        if (size_t hp = req.find(key); hp != std::string::npos) { host = req.substr(hp + strlen(key), req.find("\r\n", hp + 2) - hp - strlen(key)); break; }
    if (path == "/login") serve_login(fd, host);
    else if (path == "/callback") serve_callback(fd, q);
    else if (path == "/signout") serve_signout(fd);
    else serve_home(fd);
}
void server_main() {
    int ls = -1;
    while (g_run.load()) {
        if (ls < 0) {   /* (re)bind; another process holding the port just means retry later */
            ls = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
            int one = 1;
            setsockopt(ls, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
            sockaddr_in a{};
            a.sin_family = AF_INET; a.sin_port = htons(LOGIN_PORT); a.sin_addr.s_addr = INADDR_ANY;
            if (bind(ls, (sockaddr *)&a, sizeof a) != 0 || listen(ls, 8) != 0) {
                close(ls); ls = -1;
                for (int i = 0; i < 50 && g_run.load(); i++) std::this_thread::sleep_for(std::chrono::milliseconds(100));
                continue;
            }
        }
        pollfd p{ls, POLLIN, 0};
        if (poll(&p, 1, 250) <= 0) continue;
        int fd = accept4(ls, nullptr, nullptr, SOCK_CLOEXEC);
        if (fd < 0) continue;
        handle(fd);
        close(fd);
    }
    if (ls >= 0) close(ls);
}

}  // namespace

void start() {
    std::lock_guard<std::mutex> lk(g_mtx);
    if (g_refs++ > 0) return;
    load_config_locked();
    g_run = true;
    g_worker = std::thread(worker_main);
    g_server = std::thread(server_main);
}
void stop() {
    std::thread w, s;
    {
        std::lock_guard<std::mutex> lk(g_mtx);
        if (g_refs == 0 || --g_refs > 0) return;
        g_run = false;
        g_jobs.clear();
        w = std::move(g_worker); s = std::move(g_server);
    }
    g_cv.notify_all();
    if (w.joinable()) w.join();
    if (s.joinable()) s.join();
}

static long long next_seq_locked() {
    long long s = (long long)std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
    g_last_seq = s > g_last_seq ? s : g_last_seq + 1;
    return g_last_seq;
}
long long browse(const void *owner, int sort, int page, int page_size, const Filters &filt) {
    std::lock_guard<std::mutex> lk(g_mtx);
    for (auto it = g_jobs.begin(); it != g_jobs.end();)
        it = (!it->download && it->owner == owner) ? g_jobs.erase(it) : it + 1;
    Job jb{next_seq_locked(), owner, false, sort, page, page_size, 0, "", "", filt};
    g_jobs.push_back(jb);
    g_cv.notify_all();
    return jb.seq;
}
long long download(const void *owner, long long tone_id, const std::string &tone_name, const std::string &models_dir,
                   const Filters &filt) {
    std::lock_guard<std::mutex> lk(g_mtx);
    Job jb{next_seq_locked(), owner, true, 0, 0, 0, tone_id, tone_name, models_dir, filt};
    g_jobs.push_back(jb);
    g_cv.notify_all();
    return jb.seq;
}
Status status() {
    std::lock_guard<std::mutex> lk(g_mtx);
    return g_status;
}
bool results(long long seq, Results &out) {
    std::lock_guard<std::mutex> lk(g_mtx);
    auto it = g_results.find(seq);
    if (it == g_results.end()) return false;
    out = it->second;
    return true;
}

}  // namespace t3k
