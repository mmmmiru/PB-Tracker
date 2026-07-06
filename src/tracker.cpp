#include "tracker.h"
#include "metadata.h"
#include "db.h"
#include <inkview.h>
#include <string>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <dirent.h>
#include <unistd.h>
#include <sys/stat.h>

// Set by main.cpp when running in daemon (headless) mode.
extern bool g_daemon_mode;

// Poll interval. Shorter = snappier session boundaries but more
// wakeups/battery use; 15s is a reasonable middle ground.
static const int POLL_MS = 15000;

// Ignore sessions shorter than this -- filters out someone just
// glancing at a cover in the library, or two rapid taps.
static const int MIN_SESSION_SECONDS = 5;

// Helper: read a single line from a file and return it trimmed.
static std::string read_first_line(const char *path) {
    FILE *f = fopen(path, "r");
    if (!f) return "";
    char buf[1024] = {0};
    char *r = fgets(buf, sizeof(buf) - 1, f);
    fclose(f);
    if (!r) return "";
    std::string s(buf);
    while (!s.empty() && (s[s.length() - 1] == '\n' || s[s.length() - 1] == '\r' || s[s.length() - 1] == ' '))
        s.erase(s.length() - 1);
    return s;
}

// Check if a path looks like a book file (by extension).
static bool looks_like_book(const std::string &path) {
    if (path.empty()) return false;
    const char *exts[] = { ".epub", ".pdf", ".fb2", ".djvu", ".mobi", ".cbz", ".cbr",
                           ".txt", ".doc", ".docx", ".rtf", ".htm", ".html", ".azw",
                           ".azw3", ".prc", NULL };
    std::string lower = path;
    for (size_t i = 0; i < lower.size(); i++)
        if (lower[i] >= 'A' && lower[i] <= 'Z') lower[i] += 32;
    for (int i = 0; exts[i]; i++) {
        size_t elen = strlen(exts[i]);
        if (lower.size() >= elen && lower.compare(lower.size() - elen, elen, exts[i]) == 0)
            return true;
    }
    return false;
}

// Method 1: Classic CURRENTBOOK (/tmp/.current)
static std::string try_currentbook() {
    return read_first_line(CURRENTBOOK);
}

// Method 2: PocketBook lastopen.txt
static std::string try_lastopen() {
    // lastopen.txt records the most recently opened book path
    return read_first_line(FLASHDIR "/system/state/lastopen.txt");
}

// Method 3: Scan /proc for ANY process that has a book file open.
// We don't filter by process name because the reader binary name
// varies across firmware versions.
static std::string try_proc_scan() {
    DIR *proc = opendir("/proc");
    if (!proc) return "";

    // Get our own PID to skip ourselves
    int my_pid = getpid();
    char my_pid_str[16];
    snprintf(my_pid_str, sizeof(my_pid_str), "%d", my_pid);

    struct dirent *ent;
    while ((ent = readdir(proc)) != NULL) {
        // Only look at numeric (PID) directories
        bool is_pid = true;
        for (int i = 0; ent->d_name[i]; i++) {
            if (ent->d_name[i] < '0' || ent->d_name[i] > '9') { is_pid = false; break; }
        }
        if (!is_pid) continue;

        // Skip our own process
        if (strcmp(ent->d_name, my_pid_str) == 0) continue;

        // Scan this process's open file descriptors
        char fddir[128];
        snprintf(fddir, sizeof(fddir), "/proc/%s/fd", ent->d_name);
        DIR *fds = opendir(fddir);
        if (!fds) continue;

        struct dirent *fdent;
        while ((fdent = readdir(fds)) != NULL) {
            char linkpath[256], target[1024];
            snprintf(linkpath, sizeof(linkpath), "%s/%s", fddir, fdent->d_name);
            ssize_t len = readlink(linkpath, target, sizeof(target) - 1);
            if (len <= 0) continue;
            target[len] = '\0';
            std::string t(target);
            // Only consider files on /mnt/ext (internal or SD storage)
            if (t.compare(0, 8, "/mnt/ext") == 0 && looks_like_book(t)) {
                closedir(fds);
                closedir(proc);
                return t;
            }
        }
        closedir(fds);
    }
    closedir(proc);
    return "";
}

// Track the state across polls for lastopen.txt-based detection.
static time_t g_lastopen_mtime = 0;
static std::string g_lastopen_value;
// How many consecutive polls /proc found no book open.
static int g_no_proc_count = 0;

// Detect the currently open book using multiple methods.
static std::string current_book_path() {
    // Method 1: CURRENTBOOK (/tmp/.current) — fastest when supported
    std::string path = try_currentbook();
    if (!path.empty() && looks_like_book(path)) {
        g_no_proc_count = 0;
        return path;
    }

    // Method 2: Scan /proc for any process with a book file open.
    path = try_proc_scan();
    if (!path.empty()) {
        g_no_proc_count = 0;
        return path;
    }

    // Method 3: lastopen.txt — this is the primary method on this firmware.
    // When the file's mtime changes, a new book was opened.
    const char *lastopen_path = FLASHDIR "/system/state/lastopen.txt";
    struct stat st;
    if (stat(lastopen_path, &st) == 0) {
        if (st.st_mtime != g_lastopen_mtime) {
            // lastopen.txt just changed — a book was opened!
            g_lastopen_mtime = st.st_mtime;
            g_lastopen_value = try_lastopen();
            g_no_proc_count = 0;
            if (looks_like_book(g_lastopen_value)) {
                return g_lastopen_value;
            }
        }
    }

    // If we had a book via lastopen.txt and /proc doesn't see it,
    // give it a few poll cycles before declaring the book closed.
    // The reader process might just be between pages or loading.
    if (!g_lastopen_value.empty()) {
        g_no_proc_count++;
        // After 4 consecutive polls (~60s) with no /proc match, 
        // consider the book closed.
        if (g_no_proc_count < 4) {
            return g_lastopen_value;
        }
        // Book is truly closed now
        g_lastopen_value.clear();
    }

    return "";
}

struct ActiveSession {
    bool active;
    std::string path;
    BookMeta meta;
    time_t start_time;
    time_t last_sync_time;
    ActiveSession() : active(false), start_time(0), last_sync_time(0) {}
};

static ActiveSession g_session;

static void close_session(time_t end_time) {
    if (!g_session.active) return;
    long total_duration = (long)(end_time - g_session.start_time);
    long inc_duration = (long)(end_time - g_session.last_sync_time);

    db_log("close_session: Closing session for path '%s'. Total: %ld, Inc: %ld", 
           g_session.path.c_str(), total_duration, inc_duration);

    if (total_duration >= MIN_SESSION_SECONDS) {
        bool ok = db_update_active_session(g_session.meta, g_session.start_time, end_time, inc_duration);
        db_log("close_session: Recorded to database: %s", ok ? "SUCCESS" : "FAILED");
    } else {
        db_log("close_session: Session ignored (too short: %ld < %d seconds).", total_duration, MIN_SESSION_SECONDS);
    }
    g_session = ActiveSession();
}

static void open_session(const std::string &path, time_t start_time) {
    db_log("open_session: Starting new session for path '%s'.", path.c_str());
    g_session.active = true;
    g_session.path = path;
    // In daemon mode, avoid InkView calls that would crash.
    g_session.meta = g_daemon_mode ? metadata_read_basic(path) : metadata_read(path);
    g_session.start_time = start_time;
    g_session.last_sync_time = start_time;
    db_log("open_session: Metadata retrieved (Title: '%s', Author: '%s').", g_session.meta.title.c_str(), g_session.meta.author.c_str());
}

void tracker_poll() {
    std::string path = current_book_path();
    time_t now = time(NULL);

    db_log("tracker_poll: Polling (Path: '%s', ActiveSession: %s, ActivePath: '%s')", 
           path.c_str(), g_session.active ? "YES" : "NO", g_session.path.c_str());

    if (path.empty()) {
        // No book open right now -- if we thought one was, close it out.
        close_session(now);
        return;
    }

    if (!g_session.active) {
        open_session(path, now);
        return;
    }

    if (g_session.path != path) {
        // Reader switched to a different book without an empty gap
        // in between (e.g. jumping straight from one book to
        // another via "recent books").
        close_session(now);
        open_session(path, now);
    } else {
        // Same book still open, check if we need an incremental sync
        long inc_duration = (long)(now - g_session.last_sync_time);
        long total_duration = (long)(now - g_session.start_time);
        
        if (total_duration >= MIN_SESSION_SECONDS && inc_duration >= 15) {
            db_log("tracker_poll: Performing periodic incremental sync...");
            db_update_active_session(g_session.meta, g_session.start_time, now, inc_duration);
            g_session.last_sync_time = now;
        }
    }
}

void tracker_flush() {
    db_log("tracker_flush: Flushing current session.");
    close_session(time(NULL));
}

void tracker_init() {
    db_log("tracker_init: Initializing timer...");
    SetHardTimer("pbrs_poll", tracker_poll, POLL_MS);
    db_log("tracker_init: Timer set for %d ms. Performing initial poll.", POLL_MS);
    tracker_poll();
}
