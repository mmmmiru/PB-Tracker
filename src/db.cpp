#include <stdio.h>
#include <stdarg.h>
#include <string>
static std::string to_string_impl(int v) { char b[32]; snprintf(b, sizeof(b), "%d", v); return b; }
#include "db.h"
#include <sqlite3.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <time.h>
#include <unistd.h>
#include <map>
#include <algorithm>
#include <set>

// Helper to extract native reading progress from PocketBook's system DB
struct NativeProgress {
    float progress = 0.0f;
    bool finished = false;
};

static NativeProgress get_native_progress(const std::string& path) {
    NativeProgress p;
    sqlite3 *ex_db = NULL;
    // On device, explorer-3.db is mounted at /mnt/ext1 or /system. Fallbacks cover both.
    if (sqlite3_open_v2("/mnt/ext1/system/explorer-3/explorer-3.db", &ex_db, SQLITE_OPEN_READONLY, NULL) != SQLITE_OK) {
        if (sqlite3_open_v2("/system/explorer-3/explorer-3.db", &ex_db, SQLITE_OPEN_READONLY, NULL) != SQLITE_OK) {
            return p; // failed to open system db
        }
    }
    
    const char *sql = 
        "SELECT bs.cpage, bs.npage, bs.completed FROM files f "
        "JOIN folders fd ON f.folder_id = fd.id "
        "JOIN books_settings bs ON bs.bookid = f.book_id "
        "WHERE (fd.name || '/' || f.filename) = ? LIMIT 1;";
        
    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(ex_db, sql, -1, &stmt, NULL) == SQLITE_OK) {
        sqlite3_bind_text(stmt, 1, path.c_str(), -1, SQLITE_TRANSIENT);
        if (sqlite3_step(stmt) == SQLITE_ROW) {
            int cpage = sqlite3_column_int(stmt, 0);
            int npage = sqlite3_column_int(stmt, 1);
            int completed = sqlite3_column_int(stmt, 2);
            
            if (npage > 0) {
                p.progress = (float)cpage / (float)npage;
            } else if (completed == 1) {
                p.progress = 1.0f;
            }
            if (p.progress > 1.0f) p.progress = 1.0f;
            
            p.finished = (completed == 1) || (npage > 0 && cpage >= npage);
        }
        sqlite3_finalize(stmt);
    }
    sqlite3_close(ex_db);
    return p;
}

static sqlite3 *g_db = NULL;

static const char *MONTH_NAMES[] = {
    "Jan","Feb","Mar","Apr","May","Jun","Jul","Aug","Sep","Oct","Nov","Dec"
};

static const char *WEEKDAY_NAMES[] = {
    "Sun","Mon","Tue","Wed","Thu","Fri","Sat" // matches strftime('%w'): 0=Sunday
};

// Local-time midnight-ish ordinal for a calendar day, used purely to
// compare "is this the next consecutive day" for streak counting.
// Using noon rather than midnight sidesteps most DST-transition
// edge cases when dividing by seconds-per-day.
static long day_ordinal(int year, int month, int day) {
    struct tm t = {};
    t.tm_year = year - 1900;
    t.tm_mon = month - 1;
    t.tm_mday = day;
    t.tm_hour = 12;
    return (long)(mktime(&t) / 86400);
}

static const char *SCHEMA =
    "CREATE TABLE IF NOT EXISTS books ("
    "  path TEXT PRIMARY KEY,"
    "  title TEXT,"
    "  author TEXT,"
    "  series TEXT,"
    "  genre TEXT,"
    "  year INTEGER,"
    "  size_bytes INTEGER,"
    "  first_opened INTEGER,"
    "  last_opened INTEGER,"
    "  total_seconds INTEGER DEFAULT 0,"
    "  session_count INTEGER DEFAULT 0"
    ");"
    "CREATE TABLE IF NOT EXISTS sessions ("
    "  id INTEGER PRIMARY KEY AUTOINCREMENT,"
    "  path TEXT NOT NULL,"
    "  start_time INTEGER NOT NULL,"
    "  end_time INTEGER NOT NULL,"
    "  duration_seconds INTEGER NOT NULL"
    ");";

bool db_open(const std::string &db_path) {
    if (g_db) return true;
    if (sqlite3_open(db_path.c_str(), &g_db) != SQLITE_OK) {
        if (g_db) { sqlite3_close(g_db); g_db = NULL; }
        return false;
    }
    char *err = NULL;
    if (sqlite3_exec(g_db, SCHEMA, NULL, NULL, &err) != SQLITE_OK) {
        if (err) sqlite3_free(err);
        sqlite3_close(g_db);
        g_db = NULL;
        return false;
    }
    // Use synchronous=FULL to guarantee that periodic saves survive hard resets
    sqlite3_exec(g_db, "PRAGMA synchronous=FULL;", NULL, NULL, NULL);
    sqlite3_exec(g_db, "PRAGMA journal_mode=TRUNCATE;", NULL, NULL, NULL);
    return true;
}

void db_close() {
    if (g_db) { sqlite3_close(g_db); g_db = NULL; }
}

bool db_update_active_session(const BookMeta &meta, time_t start_time, time_t end_time, long incremental_duration) {
    if (!g_db) return false;
    if (incremental_duration <= 0) return false;

    long total_duration = (long)(end_time - start_time);

    sqlite3_exec(g_db, "BEGIN;", NULL, NULL, NULL);

    // 1. Try to update existing session
    sqlite3_stmt *stmt = NULL;
    sqlite3_prepare_v2(g_db,
        "UPDATE sessions SET end_time=?, duration_seconds=? WHERE path=? AND start_time=?;", 
        -1, &stmt, NULL);
    sqlite3_bind_int64(stmt, 1, (sqlite3_int64)end_time);
    sqlite3_bind_int64(stmt, 2, (sqlite3_int64)total_duration);
    sqlite3_bind_text(stmt, 3, meta.path.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(stmt, 4, (sqlite3_int64)start_time);
    sqlite3_step(stmt);
    int changes = sqlite3_changes(g_db);
    sqlite3_finalize(stmt);

    if (changes == 0) {
        // Doesn't exist yet, insert it
        sqlite3_prepare_v2(g_db,
            "INSERT INTO sessions (path, start_time, end_time, duration_seconds) VALUES (?, ?, ?, ?);", 
            -1, &stmt, NULL);
        sqlite3_bind_text(stmt, 1, meta.path.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_int64(stmt, 2, (sqlite3_int64)start_time);
        sqlite3_bind_int64(stmt, 3, (sqlite3_int64)end_time);
        sqlite3_bind_int64(stmt, 4, (sqlite3_int64)total_duration);
        sqlite3_step(stmt);
        sqlite3_finalize(stmt);
    }

    // 2. Update books table
    sqlite3_prepare_v2(g_db,
        "UPDATE books SET last_opened=?, total_seconds=total_seconds+? WHERE path=?;", 
        -1, &stmt, NULL);
    sqlite3_bind_int64(stmt, 1, (sqlite3_int64)end_time);
    sqlite3_bind_int64(stmt, 2, (sqlite3_int64)incremental_duration);
    sqlite3_bind_text(stmt, 3, meta.path.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_step(stmt);
    changes = sqlite3_changes(g_db);
    sqlite3_finalize(stmt);

    if (changes == 0) {
        // Doesn't exist yet, insert it
        sqlite3_prepare_v2(g_db,
            "INSERT INTO books (path, title, author, series, genre, year, size_bytes, "
            "  first_opened, last_opened, total_seconds, session_count) "
            "VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, 1);", 
            -1, &stmt, NULL);
        sqlite3_bind_text(stmt, 1, meta.path.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 2, meta.title.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 3, meta.author.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 4, meta.series.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 5, meta.genre.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_int(stmt, 6, meta.year);
        sqlite3_bind_int64(stmt, 7, (sqlite3_int64)meta.size_bytes);
        sqlite3_bind_int64(stmt, 8, (sqlite3_int64)start_time);
        sqlite3_bind_int64(stmt, 9, (sqlite3_int64)end_time);
        sqlite3_bind_int64(stmt, 10, (sqlite3_int64)incremental_duration);
        sqlite3_step(stmt);
        sqlite3_finalize(stmt);
    } else if (total_duration == incremental_duration) {
        // It existed in books, but this is the FIRST save of this session,
        // so we need to bump the session count.
        sqlite3_prepare_v2(g_db, "UPDATE books SET session_count=session_count+1 WHERE path=?;", -1, &stmt, NULL);
        sqlite3_bind_text(stmt, 1, meta.path.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_step(stmt);
        sqlite3_finalize(stmt);
    }

    sqlite3_exec(g_db, "COMMIT;", NULL, NULL, NULL);
    return true;
}

std::vector<BookTotal> db_get_book_totals(int limit) {
    std::vector<BookTotal> out;
    if (!g_db) return out;

    std::string sql =
        "SELECT path, title, author, total_seconds, session_count, last_opened "
        "FROM books ORDER BY last_opened DESC";
    if (limit > 0) sql += " LIMIT " + to_string_impl(limit);
    sql += ";";

    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(g_db, sql.c_str(), -1, &stmt, NULL) != SQLITE_OK) return out;

    while (sqlite3_step(stmt) == SQLITE_ROW) {
        BookTotal bt;
        const unsigned char *p = sqlite3_column_text(stmt, 0);
        const unsigned char *t = sqlite3_column_text(stmt, 1);
        const unsigned char *a = sqlite3_column_text(stmt, 2);
        bt.path = p ? reinterpret_cast<const char *>(p) : "";
        bt.title = t ? reinterpret_cast<const char *>(t) : "";
        bt.author = a ? reinterpret_cast<const char *>(a) : "";
        bt.total_seconds = (long)sqlite3_column_int64(stmt, 3);
        bt.session_count = sqlite3_column_int(stmt, 4);
        bt.last_read = (time_t)sqlite3_column_int64(stmt, 5);
        
        NativeProgress np = get_native_progress(bt.path);
        bt.progress = np.progress;
        bt.finished = np.finished;
        
        out.push_back(bt);
    }
    sqlite3_finalize(stmt);
    return out;
}

OverallStats db_get_overall_stats() {
    OverallStats s;
    if (!g_db) return s;

    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(g_db,
            "SELECT COALESCE(SUM(total_seconds),0), COALESCE(SUM(session_count),0), "
            "COUNT(*) FROM books;", -1, &stmt, NULL) == SQLITE_OK) {
        if (sqlite3_step(stmt) == SQLITE_ROW) {
            s.total_seconds = (long)sqlite3_column_int64(stmt, 0);
            s.total_sessions = sqlite3_column_int(stmt, 1);
            s.distinct_books = sqlite3_column_int(stmt, 2);
        }
        sqlite3_finalize(stmt);
    }
    
    // Count how many of our tracked books are natively marked as finished
    if (sqlite3_prepare_v2(g_db, "SELECT path FROM books;", -1, &stmt, NULL) == SQLITE_OK) {
        while (sqlite3_step(stmt) == SQLITE_ROW) {
            const unsigned char *p = sqlite3_column_text(stmt, 0);
            if (p) {
                std::string path = reinterpret_cast<const char *>(p);
                if (get_native_progress(path).finished) {
                    s.finished_books++;
                }
            }
        }
        sqlite3_finalize(stmt);
    }

    return s;
}

// Sessions store raw UTC unix timestamps, so grouping uses SQLite's
// 'localtime' modifier -- the calendar boundary should match the
// day/month/year the person actually experienced on the device, not
// the UTC one.
std::vector<PeriodStat> db_get_monthly_stats(int months_back) {
    std::vector<PeriodStat> out;
    if (!g_db) return out;

    sqlite3_stmt *stmt = NULL;
    const char *sql =
        "SELECT strftime('%Y', start_time, 'unixepoch', 'localtime') AS y, "
        "       strftime('%m', start_time, 'unixepoch', 'localtime') AS m, "
        "       SUM(duration_seconds), COUNT(*) "
        "FROM sessions GROUP BY y, m ORDER BY y DESC, m DESC LIMIT ?;";
    if (sqlite3_prepare_v2(g_db, sql, -1, &stmt, NULL) != SQLITE_OK) return out;
    sqlite3_bind_int(stmt, 1, months_back);

    while (sqlite3_step(stmt) == SQLITE_ROW) {
        PeriodStat p;
        const unsigned char *y = sqlite3_column_text(stmt, 0);
        int m = atoi(reinterpret_cast<const char *>(sqlite3_column_text(stmt, 1)));
        char label[32];
        int idx = (m >= 1 && m <= 12) ? m - 1 : 0;
        snprintf(label, sizeof(label), "%s %s", MONTH_NAMES[idx], y ? reinterpret_cast<const char *>(y) : "");
        p.label = label;
        p.year = y ? atoi(reinterpret_cast<const char *>(y)) : 0;
        p.month = m;
        p.total_seconds = (long)sqlite3_column_int64(stmt, 2);
        p.session_count = sqlite3_column_int(stmt, 3);
        out.push_back(p);
    }
    sqlite3_finalize(stmt);
    return out;
}

std::vector<PeriodStat> db_get_yearly_stats(int years_back) {
    std::vector<PeriodStat> out;
    if (!g_db) return out;

    sqlite3_stmt *stmt = NULL;
    const char *sql =
        "SELECT strftime('%Y', start_time, 'unixepoch', 'localtime') AS y, "
        "       SUM(duration_seconds), COUNT(*) "
        "FROM sessions GROUP BY y ORDER BY y DESC LIMIT ?;";
    if (sqlite3_prepare_v2(g_db, sql, -1, &stmt, NULL) != SQLITE_OK) return out;
    sqlite3_bind_int(stmt, 1, years_back);

    while (sqlite3_step(stmt) == SQLITE_ROW) {
        PeriodStat p;
        const unsigned char *y = sqlite3_column_text(stmt, 0);
        p.label = y ? reinterpret_cast<const char *>(y) : "";
        p.year = y ? atoi(reinterpret_cast<const char *>(y)) : 0;
        p.month = 0;
        p.total_seconds = (long)sqlite3_column_int64(stmt, 1);
        p.session_count = sqlite3_column_int(stmt, 2);
        out.push_back(p);
    }
    sqlite3_finalize(stmt);
    return out;
}

std::vector<BookPeriodEntry> db_get_books_in_period(time_t start, time_t end) {
    std::vector<BookPeriodEntry> out;
    if (!g_db) return out;

    // Joins against `books` for title/author since sessions only
    // ever store the path -- metadata lives with the book, not the
    // session, so it stays correct even if a title gets corrected
    // later (e.g. after a re-scan of the library).
    const char *sql =
        "SELECT s.path, b.title, b.author, SUM(s.duration_seconds), COUNT(*) "
        "FROM sessions s JOIN books b ON b.path = s.path "
        "WHERE s.start_time >= ? AND s.start_time < ? "
        "GROUP BY s.path ORDER BY SUM(s.duration_seconds) DESC;";

    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(g_db, sql, -1, &stmt, NULL) != SQLITE_OK) return out;
    sqlite3_bind_int64(stmt, 1, (sqlite3_int64)start);
    sqlite3_bind_int64(stmt, 2, (sqlite3_int64)end);

    while (sqlite3_step(stmt) == SQLITE_ROW) {
        BookPeriodEntry e;
        const unsigned char *path = sqlite3_column_text(stmt, 0);
        const unsigned char *t = sqlite3_column_text(stmt, 1);
        const unsigned char *a = sqlite3_column_text(stmt, 2);
        e.path = path ? reinterpret_cast<const char *>(path) : "";
        e.title = t ? reinterpret_cast<const char *>(t) : "";
        e.author = a ? reinterpret_cast<const char *>(a) : "";
        e.total_seconds = (long)sqlite3_column_int64(stmt, 3);
        e.session_count = sqlite3_column_int(stmt, 4);
        
        NativeProgress np = get_native_progress(e.path);
        e.progress = np.progress;
        e.finished = np.finished;
        
        out.push_back(e);
    }
    sqlite3_finalize(stmt);
    return out;
}

static DayStat row_to_daystat(sqlite3_stmt *stmt) {
    DayStat d;
    const unsigned char *y = sqlite3_column_text(stmt, 0);
    const unsigned char *m = sqlite3_column_text(stmt, 1);
    const unsigned char *day = sqlite3_column_text(stmt, 2);
    d.year = y ? atoi(reinterpret_cast<const char *>(y)) : 0;
    d.month = m ? atoi(reinterpret_cast<const char *>(m)) : 0;
    d.day = day ? atoi(reinterpret_cast<const char *>(day)) : 0;
    d.total_seconds = (long)sqlite3_column_int64(stmt, 3);
    d.session_count = sqlite3_column_int(stmt, 4);
    return d;
}

std::vector<DayStat> db_get_daily_stats(int days_back) {
    std::vector<DayStat> out;
    if (!g_db) return out;

    const char *sql =
        "SELECT strftime('%Y', start_time, 'unixepoch', 'localtime') AS y, "
        "       strftime('%m', start_time, 'unixepoch', 'localtime') AS m, "
        "       strftime('%d', start_time, 'unixepoch', 'localtime') AS d, "
        "       strftime('%w', start_time, 'unixepoch', 'localtime') AS w, "
        "       SUM(duration_seconds), COUNT(*) "
        "FROM sessions GROUP BY y, m, d ORDER BY y DESC, m DESC, d DESC LIMIT ?;";

    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(g_db, sql, -1, &stmt, NULL) != SQLITE_OK) return out;
    sqlite3_bind_int(stmt, 1, days_back);

    while (sqlite3_step(stmt) == SQLITE_ROW) {
        DayStat d;
        const unsigned char *y = sqlite3_column_text(stmt, 0);
        const unsigned char *m = sqlite3_column_text(stmt, 1);
        const unsigned char *day = sqlite3_column_text(stmt, 2);
        int w = atoi(reinterpret_cast<const char *>(sqlite3_column_text(stmt, 3)));
        d.year = y ? atoi(reinterpret_cast<const char *>(y)) : 0;
        d.month = m ? atoi(reinterpret_cast<const char *>(m)) : 0;
        d.day = day ? atoi(reinterpret_cast<const char *>(day)) : 0;
        d.total_seconds = (long)sqlite3_column_int64(stmt, 4);
        d.session_count = sqlite3_column_int(stmt, 5);

        char label[24];
        int widx = (w >= 0 && w <= 6) ? w : 0;
        snprintf(label, sizeof(label), "%s %d", WEEKDAY_NAMES[widx], d.day);
        d.label = label;

        out.push_back(d);
    }
    sqlite3_finalize(stmt);
    return out;
}

std::vector<DayStat> db_get_daily_totals_in_period(time_t start, time_t end) {
    std::vector<DayStat> out;
    if (!g_db) return out;

    const char *sql =
        "SELECT strftime('%Y', start_time, 'unixepoch', 'localtime') AS y, "
        "       strftime('%m', start_time, 'unixepoch', 'localtime') AS m, "
        "       strftime('%d', start_time, 'unixepoch', 'localtime') AS d, "
        "       SUM(duration_seconds), COUNT(*) "
        "FROM sessions WHERE start_time >= ? AND start_time < ? "
        "GROUP BY y, m, d ORDER BY y ASC, m ASC, d ASC;";

    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(g_db, sql, -1, &stmt, NULL) != SQLITE_OK) return out;
    sqlite3_bind_int64(stmt, 1, (sqlite3_int64)start);
    sqlite3_bind_int64(stmt, 2, (sqlite3_int64)end);

    while (sqlite3_step(stmt) == SQLITE_ROW) {
        out.push_back(row_to_daystat(stmt));
    }
    sqlite3_finalize(stmt);
    return out;
}

StreakInfo db_get_streaks() {
    StreakInfo s;
    if (!g_db) return s;

    // Every distinct day that had any reading, oldest first. No
    // LIMIT -- streaks need the full history to find the longest
    // run, and this table stays small (one row per active day, not
    // per session).
    const char *sql =
        "SELECT DISTINCT strftime('%Y', start_time, 'unixepoch', 'localtime'), "
        "                strftime('%m', start_time, 'unixepoch', 'localtime'), "
        "                strftime('%d', start_time, 'unixepoch', 'localtime') "
        "FROM sessions ORDER BY 1 ASC, 2 ASC, 3 ASC;";

    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(g_db, sql, -1, &stmt, NULL) != SQLITE_OK) return s;

    std::vector<long> ordinals;
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        int y = atoi(reinterpret_cast<const char *>(sqlite3_column_text(stmt, 0)));
        int m = atoi(reinterpret_cast<const char *>(sqlite3_column_text(stmt, 1)));
        int d = atoi(reinterpret_cast<const char *>(sqlite3_column_text(stmt, 2)));
        ordinals.push_back(day_ordinal(y, m, d));
    }
    sqlite3_finalize(stmt);
    if (ordinals.empty()) return s;

    int run = 1, best = 1;
    for (size_t i = 1; i < ordinals.size(); i++) {
        if (ordinals[i] == ordinals[i - 1] + 1) {
            run++;
        } else {
            run = 1;
        }
        best = std::max(best, run);
    }
    s.best_streak = best;

    // Current streak only counts if the most recent active day is
    // today or yesterday -- otherwise the streak is over, it's just
    // sitting at 0 until the next session.
    time_t now = time(NULL);
    struct tm *lt = localtime(&now);
    long today_ord = day_ordinal(lt->tm_year + 1900, lt->tm_mon + 1, lt->tm_mday);
    long last = ordinals.back();

    if (last == today_ord || last == today_ord - 1) {
        int cur = 1;
        for (int i = (int)ordinals.size() - 2; i >= 0; i--) {
            if (ordinals[i] == ordinals[i + 1] - 1) cur++;
            else break;
        }
        s.current_streak = cur;
    } else {
        s.current_streak = 0;
    }

    return s;
}

void db_log(const char *fmt, ...) {
    std::string log_path = std::string(FLASHDIR) + "/system/pbreadstats/debug.log";
    FILE *f = fopen(log_path.c_str(), "a");
    if (!f) return;
    time_t now = time(NULL);
    struct tm *lt = localtime(&now);
    char time_str[32];
    strftime(time_str, sizeof(time_str), "%Y-%m-%d %H:%M:%S", lt);
    fprintf(f, "[%s] ", time_str);
    va_list args;
    va_start(args, fmt);
    vfprintf(f, fmt, args);
    va_end(args);
    fprintf(f, "\n");
    fclose(f);
}
