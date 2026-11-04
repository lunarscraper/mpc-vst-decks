/* =============================================================================
 * decks_library.h - drives, music folders, the track list and the worker thread.
 *
 * Drives: every mounted file system under /media/..., /sdcard, /mnt/... or /run/media/...
 * (from /proc/mounts; system file systems left out, a device mounted twice listed once). On the
 * Force that is the internal storage, an installed SSD and an SD card or USB stick plugged in.
 * DECKS_ROOTS=/a:/b replaces the list (offline test, or a device that mounts elsewhere).
 * Sources: one music folder per drive, chosen on the SOURCE page; several drives at once.
 * Kept in decks_sources.txt ("<mount>\t<folder>" per line).
 * Track list: the source folders, searched recursively for WAV, AIFF, FLAC and MP3, sorted by
 * path. Kept in decks_index.txt with size, time and the analysis (tempo, first kick), so a
 * track is analysed once; a changed file (size or time) is analysed again. The list is read
 * again when the plugin starts, when a drive comes or goes (checked every 2 s), and on RESCAN.
 * Tracks of a source whose drive is missing stay in the list, marked OFFLINE.
 * Both files sit next to the plugin's folder (as the Rattler presets do); DECKS_DATA overrides.
 *
 * The worker thread does everything slow: mounts, folder walks, decoding (dr_wav, dr_flac,
 * dr_mp3), analysis, the files. The audio thread only sets atomics and swaps track pointers;
 * the skin's text (UI thread) reads the lists under a mutex the worker holds only briefly.
 * MIT license (see ../LICENSE).
 * ========================================================================== */
#pragma once
#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <dirent.h>
#include <mutex>
#include <string>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <unistd.h>
#include <thread>
#include <vector>

#include "decks_analysis.h"
#include "decks_engine.h"

/* the decoders: public domain / MIT-0, see third_party/ */
#if defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#pragma GCC diagnostic ignored "-Wunused-parameter"
#pragma GCC diagnostic ignored "-Wsign-compare"
#pragma GCC diagnostic ignored "-Wimplicit-fallthrough"
#pragma GCC diagnostic ignored "-Wmissing-field-initializers"
#endif
#define DR_WAV_IMPLEMENTATION
#define DR_WAV_NO_WCHAR
#include "third_party/dr_wav.h"
#define DR_FLAC_IMPLEMENTATION
#define DR_FLAC_NO_WCHAR
#include "third_party/dr_flac.h"
#define DR_MP3_IMPLEMENTATION
#define DR_MP3_NO_WCHAR
#include "third_party/dr_mp3.h"
#if defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

namespace decks {

enum { ROWS = 8, FROWS = 6, MAX_TRACKS = 20000, MAX_FOLDERS = 4000, NRETIRED = 8 };
static const double MAX_SECONDS = 20 * 60;   /* 20 min of 16-bit stereo at 44.1 kHz = 212 MB */

/* ---- small helpers ---------------------------------------------------------------- */
inline std::string lower(std::string s) { for (char &c : s) c = (char)std::tolower((unsigned char)c); return s; }
inline std::string base_name(const std::string &p) { size_t s = p.rfind('/'); return s == std::string::npos ? p : p.substr(s + 1); }
inline std::string ext_of(const std::string &p) {
    const std::string b = base_name(p);
    size_t d = b.rfind('.');
    return d == std::string::npos ? "" : lower(b.substr(d + 1));
}
inline bool is_audio(const std::string &p) {
    const std::string e = ext_of(p);
    return e == "wav" || e == "wave" || e == "aif" || e == "aiff" || e == "flac" || e == "mp3";
}
inline std::string strip_ext(const std::string &b) { size_t d = b.rfind('.'); return d == std::string::npos || d == 0 ? b : b.substr(0, d); }
/* the skin's text is shown as plain ASCII: German umlauts spelled out, other letters as '?' */
inline std::string ascii(const std::string &s, size_t max = 200) {
    std::string o;
    for (size_t i = 0; i < s.size() && o.size() < max; i++) {
        const unsigned char c = (unsigned char)s[i];
        if (c < 0x80) { o += (c < 0x20 || c == 0x7f) ? ' ' : (char)c; continue; }
        const unsigned char d = i + 1 < s.size() ? (unsigned char)s[i + 1] : 0;
        const char *rep = nullptr;
        if (c == 0xc3) switch (d) {
            case 0xa4: rep = "ae"; break; case 0xb6: rep = "oe"; break; case 0xbc: rep = "ue"; break;
            case 0x84: rep = "Ae"; break; case 0x96: rep = "Oe"; break; case 0x9c: rep = "Ue"; break;
            case 0x9f: rep = "ss"; break; case 0xa9: case 0xa8: case 0xaa: rep = "e"; break;
            case 0xa1: case 0xa0: case 0xa2: rep = "a"; break; case 0xb3: case 0xb2: case 0xb4: rep = "o"; break;
            case 0xad: case 0xac: rep = "i"; break; case 0xba: case 0xb9: rep = "u"; break; case 0xb1: rep = "n"; break;
            case 0x89: rep = "E"; break; default: break;
        }
        int len = c >= 0xf0 ? 4 : c >= 0xe0 ? 3 : c >= 0xc0 ? 2 : 1;
        o += rep ? rep : "?";
        i += (size_t)len - 1;
    }
    return o;
}
inline std::string fmt_time(double s) {
    if (s < 0) s = 0;
    char b[24];
    const int t = (int)s;
    std::snprintf(b, sizeof b, "%d:%02d", t / 60, t % 60);
    return b;
}

struct Drive { std::string mount, label, dev; };
struct Entry {
    std::string path, name, src;   /* src: the source folder it was found in */
    long long size = 0, mtime = 0;
    double bpm = -1, beat0 = 0;    /* bpm -1 = not analysed yet, 0 = no tempo */
    double secs = 0;
    bool online = true;
};

/* mounted drives (see the top of the file) */
inline std::vector<Drive> list_drives() {
    std::vector<Drive> out;
    if (const char *env = std::getenv("DECKS_ROOTS")) {
        std::string s = env;
        size_t p = 0;
        while (p <= s.size()) {
            size_t c = s.find(':', p);
            if (c == std::string::npos) c = s.size();
            std::string m = s.substr(p, c - p);
            while (m.size() > 1 && m.back() == '/') m.pop_back();
            struct stat st;
            if (!m.empty() && stat(m.c_str(), &st) == 0 && S_ISDIR(st.st_mode)) out.push_back({m, base_name(m), m});
            p = c + 1;
        }
        return out;
    }
    FILE *f = std::fopen("/proc/mounts", "r");
    if (!f) return out;
    static const char *const SKIP[] = {"proc", "sysfs", "tmpfs", "devtmpfs", "devpts", "cgroup", "cgroup2", "overlay",
                                       "squashfs", "debugfs", "securityfs", "pstore", "configfs", "fusectl", "mqueue",
                                       "tracefs", "bpf", "autofs", "ramfs", "rootfs", "binfmt_misc", "hugetlbfs"};
    char line[1024];
    while (std::fgets(line, sizeof line, f)) {
        char dev[256], mnt[512], type[64];
        if (std::sscanf(line, "%255s %511s %63s", dev, mnt, type) != 3) continue;
        bool skip = false;
        for (const char *s : SKIP) if (!std::strcmp(type, s)) skip = true;
        if (skip) continue;
        std::string m;   /* /proc/mounts escapes blanks as \040 */
        for (const char *q = mnt; *q; q++) {
            if (q[0] == '\\' && q[1] >= '0' && q[1] <= '3' && q[2] && q[3]) { m += (char)((q[1] - '0') * 64 + (q[2] - '0') * 8 + (q[3] - '0')); q += 3; }
            else m += *q;
        }
        if (!(m.rfind("/media/", 0) == 0 || m == "/sdcard" || m.rfind("/mnt/", 0) == 0 || m.rfind("/run/media/", 0) == 0)) continue;
        bool dup = false;
        for (const Drive &d : out) if (d.dev == dev || d.mount == m) dup = true;
        if (dup) continue;
        std::string label = m == "/sdcard" ? "SDCARD" : m.find("az01-internal") != std::string::npos ? "INTERNAL" : base_name(m);
        out.push_back({m, label, dev});
    }
    std::fclose(f);
    return out;
}

/* next to the plugin folder (found in /proc/self/maps), else inside it, else /tmp */
inline std::string data_dir(const char *so_name) {
    if (const char *env = std::getenv("DECKS_DATA")) return env;
    std::string dir;
    if (FILE *f = std::fopen("/proc/self/maps", "r")) {
        char line[1024];
        while (std::fgets(line, sizeof line, f)) {
            char *p = std::strstr(line, so_name);
            char *first = std::strchr(line, '/');
            if (!p || !first || first > p) continue;
            dir.assign(first, (size_t)(p - first));
            break;
        }
        std::fclose(f);
    }
    while (dir.size() > 1 && dir.back() == '/') dir.pop_back();
    if (dir.empty()) return "/tmp";
    const size_t cut = dir.rfind('/');
    const std::string parent = cut != std::string::npos && cut > 0 ? dir.substr(0, cut) : dir;
    const std::string probe = parent + "/decks_index.txt";
    if (FILE *f = std::fopen(probe.c_str(), "a")) { std::fclose(f); return parent; }
    return dir;
}

/* decodes a whole file to 16-bit stereo; nullptr + a reason on failure */
inline Track *decode(const std::string &path, std::string *why) {
    unsigned ch = 0, sr = 0;
    unsigned long long n = 0;
    int16_t *p = nullptr;
    const std::string e = ext_of(path);
    if (e == "flac") {
        p = drflac_open_file_and_read_pcm_frames_s16(path.c_str(), &ch, &sr, (drflac_uint64 *)&n, nullptr);
    } else if (e == "mp3") {
        drmp3_config cfg;
        std::memset(&cfg, 0, sizeof cfg);
        drmp3_uint64 fn = 0;
        p = drmp3_open_file_and_read_pcm_frames_s16(path.c_str(), &cfg, &fn, nullptr);
        ch = cfg.channels; sr = cfg.sampleRate; n = fn;
    } else {
        drwav_uint64 fn = 0;
        p = drwav_open_file_and_read_pcm_frames_s16(path.c_str(), &ch, &sr, &fn, nullptr);
        n = fn;
    }
    if (!p || !n || !ch || !sr) { if (p) std::free(p); *why = "CANNOT READ FILE"; return nullptr; }
    if ((double)n / sr > MAX_SECONDS) { std::free(p); *why = "LONGER THAN 20 MIN"; return nullptr; }
    if (ch != 2) {   /* mono doubled, more channels: the first two */
        int16_t *q = (int16_t *)std::malloc((size_t)n * 2 * sizeof(int16_t));
        if (!q) { std::free(p); *why = "OUT OF MEMORY"; return nullptr; }
        for (unsigned long long i = 0; i < n; i++) {
            q[2 * i] = p[i * ch];
            q[2 * i + 1] = p[i * ch + (ch > 1 ? 1 : 0)];
        }
        std::free(p);
        p = q;
    }
    Track *t = new Track();
    t->pcm = p;
    t->frames = (int64_t)n;
    t->sr = sr;
    t->release = std::free;
    t->path = path;
    t->title = strip_ext(base_name(path));
    return t;
}

struct Library {
    /* ---- shared with the audio thread (atomics only) ---- */
    std::atomic<Track *> incoming[2] = {{nullptr}, {nullptr}};   /* worker -> audio: a track to put on the deck */
    std::atomic<Track *> retired[NRETIRED];                     /* audio -> worker: tracks to free */
    std::atomic<bool> busy[2] = {{false}, {false}};               /* deck playing (set by the audio thread) */
    std::atomic<int> load_req[2] = {{0}, {0}};                    /* list index + 1 */
    std::atomic<int> sel{0};                                      /* selected track (list index) */
    std::atomic<int> count{0};                                    /* tracks in the list */
    std::atomic<int> fdrive{0};                                   /* drive on the SOURCE page */
    std::atomic<int> ftop{0};                                     /* first folder row shown */
    std::atomic<int> fcount{0};
    std::atomic<int> ndrives{0};
    std::atomic<int> enter_req{0};                                /* folder row + 1 */
    std::atomic<bool> up_req{false}, use_req{false}, remove_req{false}, rescan_req{true};
    std::atomic<bool> quit{false};
    std::atomic<int> loading[2] = {{0}, {0}};                     /* 1 while a deck's track is being read */
    std::atomic<int> sel_changed{0};                              /* the worker moved the selection (rescan) */

    /* ---- for the skin's text (UI thread) and the worker, under mtx ---- */
    std::mutex mtx;
    std::vector<Drive> drives;
    std::vector<Entry> entries;
    std::vector<std::string> folders;       /* subfolders of fpath */
    std::string fpath;                      /* folder shown on the SOURCE page */
    int fpath_drive = -1;
    std::vector<std::pair<std::string, std::string>> sources;   /* mount, folder */
    std::string status = "STARTING";
    std::string deck_path[2];               /* what is on each deck (for the chunk and the [A]/[B] marks) */
    double deck_bpm[2] = {0, 0}, deck_secs[2] = {0, 0};
    std::string path_req[2];                /* a path to load (project restore) */
    std::string dir;

    std::thread th;

    void start(const std::string &data) {
        for (auto &r : retired) r.store(nullptr);
        dir = data;
        th = std::thread([this] { run(); });
    }
    void stop() {
        quit.store(true);
        if (th.joinable()) th.join();
        for (auto &r : retired) delete r.exchange(nullptr);
        for (auto &i : incoming) delete i.exchange(nullptr);
    }
    void set_status(const std::string &s) { std::lock_guard<std::mutex> lk(mtx); status = s; }

    /* audio thread: hand back a track that left a deck; false = all slots full, try next block */
    bool retire(Track *t) {
        if (!t) return true;
        for (auto &r : retired) {
            Track *expect = nullptr;
            if (r.compare_exchange_strong(expect, t)) return true;
        }
        return false;
    }

    /* ---- files ---- */
    std::string f_sources() const { return dir + "/decks_sources.txt"; }
    std::string f_index() const { return dir + "/decks_index.txt"; }
    static bool write_file(const std::string &path, const std::string &text) {
        const std::string tmp = path + ".tmp";
        FILE *f = std::fopen(tmp.c_str(), "w");
        if (!f) return false;
        std::fwrite(text.data(), 1, text.size(), f);
        return std::fclose(f) == 0 && std::rename(tmp.c_str(), path.c_str()) == 0;
    }
    void load_files() {
        std::lock_guard<std::mutex> lk(mtx);
        sources.clear();
        entries.clear();
        char line[4096];
        if (FILE *f = std::fopen(f_sources().c_str(), "r")) {
            while (std::fgets(line, sizeof line, f)) {
                line[std::strcspn(line, "\r\n")] = 0;
                char *t = std::strchr(line, '\t');
                if (!t) continue;
                *t = 0;
                sources.push_back({line, t + 1});
            }
            std::fclose(f);
        }
        if (FILE *f = std::fopen(f_index().c_str(), "r")) {   /* path size mtime bpm beat0 secs src */
            while (std::fgets(line, sizeof line, f)) {
                line[std::strcspn(line, "\r\n")] = 0;
                std::vector<char *> col;
                for (char *p = line;;) {
                    col.push_back(p);
                    char *q = std::strchr(p, '\t');
                    if (!q) break;
                    *q = 0;
                    p = q + 1;
                }
                if (col.size() < 7) continue;
                Entry e;
                e.path = col[0]; e.size = std::atoll(col[1]); e.mtime = std::atoll(col[2]);
                e.bpm = std::atof(col[3]); e.beat0 = std::atof(col[4]); e.secs = std::atof(col[5]); e.src = col[6];
                e.name = strip_ext(base_name(e.path));
                entries.push_back(e);
            }
            std::fclose(f);
        }
        count.store((int)entries.size());
    }
    void save_sources_locked() {
        std::string t;
        for (auto &s : sources) t += s.first + "\t" + s.second + "\n";
        write_file(f_sources(), t);
    }
    void save_index_locked() {
        std::string t;
        char b[128];
        for (const Entry &e : entries) {
            std::snprintf(b, sizeof b, "\t%lld\t%lld\t%.4f\t%.2f\t%.2f\t", e.size, e.mtime, e.bpm, e.beat0, e.secs);
            t += e.path + b + e.src + "\n";
        }
        write_file(f_index(), t);
    }

    /* ---- scanning ---- */
    static void walk(const std::string &d, int depth, const std::string &src, std::vector<Entry> &out) {
        if (depth > 12 || out.size() >= MAX_TRACKS) return;
        DIR *dp = opendir(d.c_str());
        if (!dp) return;
        std::vector<std::string> subs;
        while (struct dirent *de = readdir(dp)) {
            if (de->d_name[0] == '.') continue;   /* hidden files, macOS "._" files */
            const std::string p = d + "/" + de->d_name;
            struct stat st;
            if (lstat(p.c_str(), &st) != 0) continue;   /* symlinks are not followed: no loops */
            if (S_ISDIR(st.st_mode)) subs.push_back(p);
            else if (S_ISREG(st.st_mode) && is_audio(p) && out.size() < MAX_TRACKS) {
                Entry e;
                e.path = p; e.src = src; e.size = (long long)st.st_size; e.mtime = (long long)st.st_mtime;
                e.name = strip_ext(base_name(p));
                out.push_back(e);
            }
        }
        closedir(dp);
        for (const std::string &s : subs) walk(s, depth + 1, src, out);
    }
    void scan() {
        std::vector<std::pair<std::string, std::string>> srcs;
        std::vector<Drive> dv;
        std::vector<Entry> old;
        std::string selpath;
        {
            std::lock_guard<std::mutex> lk(mtx);
            srcs = sources; dv = drives; old = entries;
            const int s = sel.load();
            if (s >= 0 && s < (int)entries.size()) selpath = entries[(size_t)s].path;
            status = "SCANNING...";
        }
        std::vector<Entry> fresh;
        for (auto &s : srcs) {
            bool online = false;
            for (const Drive &d : dv) if (d.mount == s.first) online = true;
            if (online) {
                struct stat st;
                if (stat(s.second.c_str(), &st) == 0 && S_ISDIR(st.st_mode)) walk(s.second, 0, s.second, fresh);
            } else {   /* drive missing: keep what the index knew, marked offline */
                for (const Entry &e : old) if (e.src == s.second) { Entry c = e; c.online = false; fresh.push_back(c); }
            }
            if (quit.load()) return;
        }
        /* keep the analysis of files that did not change */
        std::vector<const Entry *> byp;
        for (const Entry &e : old) byp.push_back(&e);
        std::sort(byp.begin(), byp.end(), [](const Entry *a, const Entry *b) { return a->path < b->path; });
        for (Entry &e : fresh) {
            if (!e.online) continue;
            auto it = std::lower_bound(byp.begin(), byp.end(), e.path, [](const Entry *a, const std::string &p) { return a->path < p; });
            if (it != byp.end() && (*it)->path == e.path && (*it)->size == e.size && (*it)->mtime == e.mtime) {
                e.bpm = (*it)->bpm; e.beat0 = (*it)->beat0; e.secs = (*it)->secs;
            }
        }
        std::sort(fresh.begin(), fresh.end(), [](const Entry &a, const Entry &b) {
            const std::string x = lower(a.path), y = lower(b.path);
            return x != y ? x < y : a.path < b.path;
        });
        std::lock_guard<std::mutex> lk(mtx);
        entries.swap(fresh);
        int ns = 0;
        for (size_t i = 0; i < entries.size(); i++) if (entries[i].path == selpath) ns = (int)i;
        count.store((int)entries.size());
        sel.store(ns);
        sel_changed.store(1);
        save_index_locked();
        int off = 0;
        for (const Entry &e : entries) off += !e.online;
        char b[96];
        if (sources.empty()) std::snprintf(b, sizeof b, "NO MUSIC FOLDER - CHOOSE ONE ON THE SOURCE PAGE");
        else if (off) std::snprintf(b, sizeof b, "%d TRACKS (%d OFFLINE)", (int)entries.size(), off);
        else std::snprintf(b, sizeof b, "%d TRACKS", (int)entries.size());
        status = b;
    }

    /* ---- the SOURCE page's folder browser ---- */
    void list_folders_locked() {
        folders.clear();
        if (DIR *dp = opendir(fpath.c_str())) {
            while (struct dirent *de = readdir(dp)) {
                if (de->d_name[0] == '.' || folders.size() >= MAX_FOLDERS) continue;
                const std::string p = fpath + "/" + de->d_name;
                struct stat st;
                if (lstat(p.c_str(), &st) == 0 && S_ISDIR(st.st_mode)) folders.push_back(de->d_name);
            }
            closedir(dp);
        }
        std::sort(folders.begin(), folders.end(), [](const std::string &a, const std::string &b) { return lower(a) < lower(b); });
        fcount.store((int)folders.size());
        ftop.store(0);
    }
    /* the folder browser follows the drive chosen; it opens at that drive's source folder */
    void sync_folder_view_locked() {
        const int d = fdrive.load();
        if (d == fpath_drive && d >= 0 && d < (int)drives.size() && fpath.rfind(drives[(size_t)d].mount, 0) == 0) return;
        fpath_drive = d;
        fpath.clear();
        if (d < 0 || d >= (int)drives.size()) { folders.clear(); fcount.store(0); return; }
        fpath = drives[(size_t)d].mount;
        for (auto &s : sources) if (s.first == fpath) {
            struct stat st;
            if (stat(s.second.c_str(), &st) == 0 && S_ISDIR(st.st_mode)) fpath = s.second;
        }
        list_folders_locked();
    }
    void folder_requests() {
        const int en = enter_req.exchange(0);
        const bool up = up_req.exchange(false), use = use_req.exchange(false), rm = remove_req.exchange(false);
        bool rescan = false;
        {
            std::lock_guard<std::mutex> lk(mtx);
            sync_folder_view_locked();
            const int d = fpath_drive;
            if (d < 0 || d >= (int)drives.size()) return;
            const std::string mount = drives[(size_t)d].mount;
            if (up && fpath.size() > mount.size()) {
                fpath = fpath.substr(0, fpath.rfind('/'));
                if (fpath.size() < mount.size()) fpath = mount;
                list_folders_locked();
            }
            if (en > 0) {
                const int i = ftop.load() + en - 1;
                if (i >= 0 && i < (int)folders.size()) { fpath += "/" + folders[(size_t)i]; list_folders_locked(); }
            }
            if (use) {
                bool found = false;
                for (auto &s : sources) if (s.first == mount) { s.second = fpath; found = true; }
                if (!found) sources.push_back({mount, fpath});
                save_sources_locked();
                rescan = true;
            }
            if (rm) {
                sources.erase(std::remove_if(sources.begin(), sources.end(), [&](const std::pair<std::string, std::string> &s) { return s.first == mount; }), sources.end());
                save_sources_locked();
                rescan = true;
            }
        }
        if (rescan) scan();
    }

    /* ---- loading ---- */
    void load(int d, const std::string &path_in, int index) {
        std::string path = path_in;
        double bpm = -1, beat0 = 0;
        long long size = 0, mtime = 0;
        {
            std::lock_guard<std::mutex> lk(mtx);
            if (path.empty()) {
                if (index < 0 || index >= (int)entries.size()) return;
                const Entry &e = entries[(size_t)index];
                if (!e.online) { status = "OFFLINE: " + ascii(e.name, 60); return; }
                path = e.path;
            }
            for (const Entry &e : entries) if (e.path == path) { bpm = e.bpm; beat0 = e.beat0; size = e.size; mtime = e.mtime; }
        }
        if (busy[d].load()) { set_status(std::string("DECK ") + (char)('A' + d) + " IS PLAYING - STOP IT FIRST"); return; }
        struct stat st;
        if (stat(path.c_str(), &st) != 0) { set_status("NOT FOUND: " + ascii(base_name(path), 60)); return; }
        if ((long long)st.st_size != size || (long long)st.st_mtime != mtime) bpm = -1;   /* changed since the analysis */
        loading[d].store(1);
        set_status(std::string("LOADING ") + (char)('A' + d) + ": " + ascii(strip_ext(base_name(path)), 60));
        std::string why;
        Track *t = decode(path, &why);
        if (!t) { loading[d].store(0); set_status(why + ": " + ascii(base_name(path), 60)); return; }
        if (bpm < 0) {
            set_status(std::string("ANALYSING ") + (char)('A' + d) + ": " + ascii(t->title, 60));
            const Grid g = analyse(t->pcm, t->frames, t->sr, &quit);
            if (quit.load()) { delete t; loading[d].store(0); return; }
            bpm = g.bpm; beat0 = g.beat0;
            std::lock_guard<std::mutex> lk(mtx);
            for (Entry &e : entries) if (e.path == path) {
                e.bpm = bpm; e.beat0 = beat0; e.secs = t->frames / t->sr;
                e.size = (long long)st.st_size; e.mtime = (long long)st.st_mtime;
            }
            save_index_locked();
        }
        t->bpm = bpm > 0 ? bpm : 0;
        t->beat0 = bpm > 0 ? beat0 : 0;
        delete incoming[d].exchange(t);   /* one the audio thread never took: free it here */
        {
            std::lock_guard<std::mutex> lk(mtx);
            deck_path[d] = path;
            deck_bpm[d] = t->bpm;
            deck_secs[d] = t->frames / t->sr;
            char b[64];
            if (t->bpm > 0) std::snprintf(b, sizeof b, " (%.2f BPM)", t->bpm);
            else std::snprintf(b, sizeof b, " (NO TEMPO FOUND)");
            status = std::string("DECK ") + (char)('A' + d) + ": " + ascii(t->title, 50) + b;
        }
        loading[d].store(0);
    }

    void run() {
        setpriority(PRIO_PROCESS, (id_t)syscall(SYS_gettid), 10);   /* below MPC's own threads: decoding may take its time */
        load_files();
        std::string last_mounts = "?";
        auto next_mounts = std::chrono::steady_clock::now();
        while (!quit.load()) {
            for (auto &r : retired) delete r.exchange(nullptr);
            const auto now = std::chrono::steady_clock::now();
            if (now >= next_mounts) {
                next_mounts = now + std::chrono::seconds(2);
                std::vector<Drive> dv = list_drives();
                std::string key;
                for (const Drive &d : dv) key += d.mount + "\n";
                if (key != last_mounts) {
                    last_mounts = key;
                    {
                        std::lock_guard<std::mutex> lk(mtx);
                        drives = dv;
                        ndrives.store((int)dv.size());
                        fpath_drive = -1;   /* re-open the folder view on the (maybe moved) drive */
                        if (fdrive.load() >= (int)drives.size()) fdrive.store(0);
                    }
                    rescan_req.store(true);
                }
            }
            folder_requests();
            if (rescan_req.exchange(false)) scan();
            for (int d = 0; d < 2; d++) {
                std::string p;
                { std::lock_guard<std::mutex> lk(mtx); p.swap(path_req[d]); }
                if (!p.empty()) load(d, p, -1);
                const int r = load_req[d].exchange(0);
                if (r > 0) load(d, "", r - 1);
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
    }
};

}  // namespace decks
