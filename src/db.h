#ifndef PBRS_DB_H
#define PBRS_DB_H

#include <string>
#include <vector>
#include "metadata.h"

// Opens (creating if necessary) the SQLite database and its schema.
// Safe to call multiple times. Returns false if the DB couldn't be
// opened (e.g. storage not mounted yet).
bool db_open(const std::string &db_path);

void db_close();

// Write debug info to a local log file on the device
void db_log(const char *fmt, ...);

// Records a reading session incrementally. `incremental_duration` is the amount
// of time elapsed since the last time this function was called for this session.
// This allows the daemon to save reading progress continuously.
bool db_update_active_session(const BookMeta &meta, time_t start_time, time_t end_time, long incremental_duration);

struct BookTotal {
    std::string path;
    std::string title;
    std::string author;
    long total_seconds = 0;
    int session_count = 0;
    float progress = 0.0f;
    bool finished = false;
    time_t last_read = 0;
};

// Returns per-book totals, most-recently-read first. `limit` <= 0
// means "no limit".
std::vector<BookTotal> db_get_book_totals(int limit = 20);

struct OverallStats {
    long total_seconds = 0;
    int total_sessions = 0;
    int distinct_books = 0;
    int finished_books = 0;
};

OverallStats db_get_overall_stats();

// One row of aggregated reading time over a calendar period.
// `label` is a ready-to-display string ("Jul 2026" or "2026").
// `year`/`month` are the raw values (month is 1-12, or 0 for a
// yearly row) so callers can compute exact period boundaries
// without re-parsing the label.
struct PeriodStat {
    std::string label;
    int year = 0;
    int month = 0; // 0 for yearly rows
    long total_seconds = 0;
    int session_count = 0;
};

// Most-recent-first, grouped by calendar month in local time.
// `months_back` caps how many months of history come back.
std::vector<PeriodStat> db_get_monthly_stats(int months_back = 12);

// Most-recent-first, grouped by calendar year in local time.
std::vector<PeriodStat> db_get_yearly_stats(int years_back = 6);

// One book's contribution within a specific time window -- used to
// answer "which books, and how many, did I read this month/year."
struct BookPeriodEntry {
    std::string path;
    std::string title;
    std::string author;
    long total_seconds = 0;
    int session_count = 0;
    float progress = 0.0f;
    bool finished = false;
};

// Books with at least one session inside [start, end), ordered by
// time spent (most-read first). `start`/`end` are unix timestamps.
std::vector<BookPeriodEntry> db_get_books_in_period(time_t start, time_t end);

// One calendar day's totals, in local time.
struct DayStat {
    int year = 0;
    int month = 0;  // 1-12
    int day = 0;    // 1-31
    std::string label; // e.g. "Mon 6", ready to display on a bar chart
    long total_seconds = 0;
    int session_count = 0;
};

// Most-recent-first, one row per day that had at least one session,
// capped at `days_back` calendar days of history. Used for the
// "last N days" bar chart.
std::vector<DayStat> db_get_daily_stats(int days_back = 14);

// Same shape, but every day is scoped to [start, end) and returned
// oldest-first -- built for populating a calendar grid where you
// need to look up "how much on the 14th," not just a recent-first list.
std::vector<DayStat> db_get_daily_totals_in_period(time_t start, time_t end);

struct StreakInfo {
    int current_streak = 0; // consecutive days up to today/yesterday
    int best_streak = 0;    // longest run anywhere in history
};

// Computed from every distinct day that has at least one session --
// a "day read" is binary (any reading counts), same definition
// KOReader's stats plugin uses for its streak counter.
StreakInfo db_get_streaks();

#endif
