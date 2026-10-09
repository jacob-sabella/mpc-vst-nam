/* t3k.h -- Tone3000 client living inside nam_vst.so: OAuth PKCE sign-in (served from a tiny
 * built-in web page on LAN port 8090), tone search, and capture download.
 *
 * One process-wide service shared by every plugin instance (one login, one worker, one port).
 * Instances call start()/stop() in pairs; the threads exit once the last instance stops, so the
 * .so can be unloaded safely. Every request returns a seq; status().seq names the request the
 * worker last worked on, so an instance can tell an answer to ITS latest request from an older one.
 */
#pragma once
#include <string>
#include <vector>

namespace t3k {

struct Item { long long id = 0; std::string name; };

struct Results {
    long long seq = 0;
    int total_pages = 1;
    std::vector<Item> items;
};

struct Status {
    long long seq = 0;          /* request the state/message below belong to */
    bool signed_in = false;
    std::string login_url;      /* http://<lan-ip>:8090 -- open on a phone to sign in */
    std::string state = "idle"; /* idle | searching | downloading | error */
    std::string message;        /* error text, "i/N" progress, or the saved count after a download */
    std::string last_download;  /* folder (pack) or file the last download landed in, relative to models */
};

/* Search filters, all optional (empty/zero = don't constrain). Passed straight through to the API's
 * /tones/search query. Ignored for the favorites sort, which has no filter support. */
struct Filters {
    std::string gears;         /* one of amp|amp-cab|pedal|outboard|cab|space|experimental, "" = any */
    std::string sizes;         /* hyphen-joined subset of standard|lite|feather|nano|custom, "" = any */
    int architecture = 0;      /* 0 = don't send (API default: A1+custom, excludes A2); 2 = A2 only */
    bool calibrated = false;   /* only tones with a calibrated model */
    bool verified = false;     /* only tones from verified creators */
    std::string make;          /* exact make/model name, "" = any */
};

void start();
void stop();

/* sort: 0 trending, 1 newest, 2 most downloaded, 3 the user's favorited tones. page is 1-based.
 * A newer browse() from the same owner replaces one of its own that hasn't started yet. */
long long browse(const void *owner, int sort, int page, int page_size, const Filters &filt = {});
/* Saves the captures of tone_id that match filt's architecture and sizes: a single capture into
 * models_dir, several into models_dir/<tone name>/. */
long long download(const void *owner, long long tone_id, const std::string &tone_name, const std::string &models_dir,
                   const Filters &filt = {});

Status status();
bool results(long long seq, Results &out);   /* false until the worker has answered that seq */

}  // namespace t3k
