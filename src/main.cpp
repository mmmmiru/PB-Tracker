// pbreadstats: one app that does both jobs.
//
// - On launch it opens the database and starts the CURRENTBOOK poll
//   timer (see tracker.cpp) so sessions get logged even while this
//   app isn't the one on screen.
// - Whenever it's brought to the foreground, it redraws the stats
//   screen from the same database.
//
// Screen layout is a flat, swipeable sequence of pages:
//   Overview -> Jul 2026 -> Jun 2026 -> ... -> 2026 -> 2025 -> ...
// Tap the left/right edge of the screen (or use physical left/right
// keys, if present) to move through it. Each month/year page lists
// exactly which books were read in that period, with cover
// thumbnails pulled from the device's own library index.
//
// Caveat carried over from earlier versions: the timer only runs
// while this process is alive. See README.md for how to check
// whether your firmware keeps background apps running.

#include <inkview.h>
#include "tracker.h"
#include "db.h"
#include "metadata.h"
#include <string>
#include <vector>
#include <cstdio>
#include <ctime>
#include <algorithm>
#include <unistd.h>
#include <signal.h>
#include <cstdlib>
#include <cstring>

static const char *DB_DIR  = FLASHDIR "/system/pbreadstats";
static const char *DB_PATH = FLASHDIR "/system/pbreadstats/reading_stats.db";

bool g_daemon_mode = false;

static ifont *g_font_title = NULL;
static ifont *g_font_body = NULL;
static ifont *g_font_small = NULL;
static ifont *g_font_huge = NULL;

static const int COVER_W = 50;
static const int COVER_H = 70;

enum PageKind { PAGE_OVERVIEW, PAGE_CALENDAR, PAGE_MONTH, PAGE_YEAR };

struct PageDesc {
    PageKind kind;
    int year = 0;
    int month = 0; // 1-12, only meaningful for PAGE_MONTH
    std::string label;
};

static std::vector<PageDesc> g_pages;
static int g_page_index = 0;

static std::string format_hms(long seconds) {
    long h = seconds / 3600;
    long m = (seconds % 3600) / 60;
    char buf[64];
    snprintf(buf, sizeof(buf), "%ldh %02ldm", h, m);
    return buf;
}

// Local-time boundaries for a calendar month/year, as unix
// timestamps -- matches the 'localtime' grouping used in db.cpp so
// the book list here lines up exactly with the bar totals.
static time_t month_start(int year, int month) {
    struct tm t = {};
    t.tm_year = year - 1900;
    t.tm_mon = month - 1;
    t.tm_mday = 1;
    return mktime(&t);
}
static time_t year_start(int year) {
    return month_start(year, 1);
}

static const char *MONTH_NAME(int month) {
    static const char *names[] = {
        "Jan","Feb","Mar","Apr","May","Jun","Jul","Aug","Sep","Oct","Nov","Dec"
    };
    return (month >= 1 && month <= 12) ? names[month - 1] : "";
}

// Rebuilds the flat page list from whatever's in the database right
// now. Cheap enough to call every time the screen is shown, and
// keeps a freshly-finished session's month/year visible immediately.
static void rebuild_pages() {
    std::string current_label = (!g_pages.empty() && g_page_index < (int)g_pages.size())
        ? g_pages[g_page_index].label : "";

    g_pages.clear();
    g_pages.push_back({PAGE_OVERVIEW, 0, 0, "Overview"});
    g_pages.push_back({PAGE_CALENDAR, 0, 0, "Calendar"});

    for (const auto &m : db_get_monthly_stats(12)) {
        g_pages.push_back({PAGE_MONTH, m.year, m.month, m.label});
    }
    for (const auto &y : db_get_yearly_stats(6)) {
        g_pages.push_back({PAGE_YEAR, y.year, 0, y.label});
    }

    // Try to stay on the same page across a rebuild; fall back to
    // the overview if it disappeared (shouldn't normally happen).
    g_page_index = 0;
    for (size_t i = 0; i < g_pages.size(); i++) {
        if (g_pages[i].label == current_label) { g_page_index = (int)i; break; }
    }
}

static double g_scale = 1.0;

// Kobo-style scaled integer helper
static int S(int px) { return (int)(px * g_scale); }

static void draw_header(const std::string &title) {
    int h = S(50);
    FillArea(0, 0, ScreenWidth(), h, WHITE);
    DrawLine(0, h - 1, ScreenWidth(), h - 1, LGRAY);
    SetFont(g_font_title, BLACK);
    int tw = StringWidth(title.c_str());
    DrawString((ScreenWidth() - tw) / 2, S(12), title.c_str());
}

static void draw_footer_pageinfo() {
    int fh = S(28);
    int fy = ScreenHeight() - fh;
    DrawLine(S(30), fy, ScreenWidth() - S(30), fy, LGRAY);
    char line[64];
    snprintf(line, sizeof(line), "%d / %d", g_page_index + 1, (int)g_pages.size());
    SetFont(g_font_small, DGRAY);
    int w = StringWidth(line);
    DrawString((ScreenWidth() - w) / 2, fy + S(6), line);
}

static void draw_divider(int y) {
    DrawLine(S(40), y, ScreenWidth() - S(40), y, LGRAY);
}

static void draw_overview() {
    draw_header("Reading Life");

    OverallStats overall = db_get_overall_stats();

    time_t now = time(NULL);
    struct tm *lt = localtime(&now);
    int cur_year = lt->tm_year + 1900;
    int cur_month = lt->tm_mon + 1;

    time_t m_start = month_start(cur_year, cur_month);
    time_t m_end = (cur_month == 12) ? month_start(cur_year + 1, 1) : month_start(cur_year, cur_month + 1);
    std::vector<BookPeriodEntry> month_books = db_get_books_in_period(m_start, m_end);
    long month_secs = 0;
    for (const auto &b : month_books) month_secs += b.total_seconds;

    time_t y_start = year_start(cur_year);
    time_t y_end = year_start(cur_year + 1);
    std::vector<BookPeriodEntry> year_books = db_get_books_in_period(y_start, y_end);
    long year_secs = 0;
    for (const auto &b : year_books) year_secs += b.total_seconds;

    StreakInfo streak = db_get_streaks();

    int y = S(80);
    int cx = ScreenWidth() / 2;

    // === Hero: Total hours (big centered number) ===
    {
        long total_h = overall.total_seconds / 3600;
        char val[32];
        snprintf(val, sizeof(val), "%ld", total_h);
        SetFont(g_font_huge, BLACK);
        int vw = StringWidth(val);
        DrawString(cx - vw / 2, y, val);
        y += S(70);
        
        SetFont(g_font_title, BLACK);
        const char *lbl_main = "Hours";
        int lw_main = StringWidth(lbl_main);
        DrawString(cx - lw_main / 2, y, lbl_main);
        y += S(30);

        SetFont(g_font_body, DGRAY);
        const char *lbl = "Total Hours Read";
        int lw = StringWidth(lbl);
        DrawString(cx - lw / 2, y, lbl);
        y += S(40);
    }

    draw_divider(y);
    y += S(24);

    // === Three-column stats row ===
    {
        int col_w = ScreenWidth() / 3;
        int c1 = col_w / 2;
        int c2 = col_w + col_w / 2;
        int c3 = col_w * 2 + col_w / 2;
        char buf[32];

        snprintf(buf, sizeof(buf), "%d", overall.finished_books);
        SetFont(g_font_title, BLACK);
        int bw = StringWidth(buf);
        DrawString(c1 - bw / 2, y, buf);

        snprintf(buf, sizeof(buf), "%d", overall.total_sessions);
        bw = StringWidth(buf);
        DrawString(c2 - bw / 2, y, buf);

        snprintf(buf, sizeof(buf), "%d", streak.current_streak);
        bw = StringWidth(buf);
        DrawString(c3 - bw / 2, y, buf);

        int label_y = y + S(32);
        SetFont(g_font_body, DGRAY);
        const char *l1 = "Books Finished";
        int lw = StringWidth(l1);
        DrawString(c1 - lw / 2, label_y, l1);
        
        const char *l2 = "Sessions";
        lw = StringWidth(l2);
        DrawString(c2 - lw / 2, label_y, l2);
        
        const char *l3 = "Day Streak";
        lw = StringWidth(l3);
        DrawString(c3 - lw / 2, label_y, l3);

        // Vertical dividers
        DrawLine(col_w, y - S(10), col_w, label_y + S(20), BLACK);
        DrawLine(col_w * 2, y - S(10), col_w * 2, label_y + S(20), BLACK);

        y = label_y + S(40);
    }

    draw_divider(y);
    y += S(16);

    // === This Month / This Year ===
    {
        int half = ScreenWidth() / 2;
        int c1 = half / 2;
        int c2 = half + half / 2;

        SetFont(g_font_small, DGRAY);
        const char *ml = "This Month";
        int lw = StringWidth(ml);
        DrawString(c1 - lw / 2, y, ml);
        const char *yl = "This Year";
        lw = StringWidth(yl);
        DrawString(c2 - lw / 2, y, yl);
        y += S(20);

        SetFont(g_font_body, BLACK);
        std::string ms = format_hms(month_secs);
        int mw = StringWidth(ms.c_str());
        DrawString(c1 - mw / 2, y, ms.c_str());
        std::string ys = format_hms(year_secs);
        int yw = StringWidth(ys.c_str());
        DrawString(c2 - yw / 2, y, ys.c_str());
        y += S(22);

        SetFont(g_font_small, DGRAY);
        char mbuf[32], ybuf[32];
        snprintf(mbuf, sizeof(mbuf), "%d book%s", (int)month_books.size(), month_books.size() == 1 ? "" : "s");
        snprintf(ybuf, sizeof(ybuf), "%d book%s", (int)year_books.size(), year_books.size() == 1 ? "" : "s");
        mw = StringWidth(mbuf);
        DrawString(c1 - mw / 2, y, mbuf);
        yw = StringWidth(ybuf);
        DrawString(c2 - yw / 2, y, ybuf);

        DrawLine(half, y - S(46), half, y + S(14), LGRAY);
        y += S(28);
    }

    draw_divider(y);
    y += S(16);

    // === Recent Books ===
    SetFont(g_font_title, BLACK);
    DrawString(S(24), y, "Recent Books");
    y += S(32);

    std::vector<BookTotal> books = db_get_book_totals(4);

    int cover_w = S(70);
    int cover_h = S(100);
    int card_w = (ScreenWidth() - S(72)) / 2; // Two columns with padding
    int card_h = cover_h + S(32);
    int margin = S(24);
    int row_gap = S(20);
    int col_gap = S(24);
    
    for (size_t i = 0; i < books.size(); i++) {
        const auto &b = books[i];
        int row = i / 2;
        int col = i % 2;
        
        int card_x = margin + col * (card_w + col_gap);
        int card_y = y + row * (card_h + row_gap);
        
        if (card_y + card_h > ScreenHeight() - S(36)) break;

        // Draw Card Border
        DrawRect(card_x, card_y, card_w, card_h, LGRAY);
        // Optional: Inner rounded effect can be simulated, but plain rect is Kobo-like

        int item_y = card_y + S(16);
        int item_x = card_x + S(16);

        ibitmap *cover = metadata_cover(b.path, cover_w, cover_h);
        if (cover) {
            DrawBitmap(item_x, item_y, cover);
        } else {
            FillArea(item_x, item_y, cover_w, cover_h, LGRAY);
            DrawRect(item_x, item_y, cover_w, cover_h, DGRAY);
        }

        int text_x = item_x + cover_w + S(14);
        int max_tw = card_w - cover_w - S(46);

        SetFont(g_font_body, BLACK);
        std::string title = b.title.empty() ? "(untitled)" : b.title;
        // Truncate title if it's too long
        while (title.length() > 3 && StringWidth(title.c_str()) > max_tw) {
            title = title.substr(0, title.length() - 4) + "...";
        }
        DrawString(text_x, item_y + S(4), title.c_str());

        SetFont(g_font_small, DGRAY);
        std::string author = b.author.empty() ? "" : b.author;
        while (author.length() > 3 && StringWidth(author.c_str()) > max_tw) {
            author = author.substr(0, author.length() - 4) + "...";
        }
        if (!author.empty()) {
            DrawString(text_x, item_y + S(28), author.c_str());
        }

        char detail[128];
        if (b.finished) {
            snprintf(detail, sizeof(detail), "Finished");
        } else {
            snprintf(detail, sizeof(detail), "%d%% read", (int)(b.progress * 100));
        }
        DrawString(text_x, item_y + S(56), detail);

        // Draw Progress Bar
        int bar_y = item_y + S(82);
        int bar_w = max_tw;
        int bar_h = S(6);
        
        float progress = b.progress;
        
        DrawRect(text_x, bar_y, bar_w, bar_h, DGRAY);
        FillArea(text_x + 1, bar_y + 1, (int)((bar_w - 2) * progress), bar_h - 2, BLACK);
    }
    
    if (!books.empty()) {
        int rows = (books.size() + 1) / 2;
        y += rows * (card_h + row_gap);
    }

    if (books.empty()) {
        SetFont(g_font_small, DGRAY);
        const char *msg = "No books recorded yet. Start reading!";
        int mw = StringWidth(msg);
        DrawString(cx - mw / 2, y, msg);
    }
}

static void draw_period_detail(const std::string &header, time_t start, time_t end) {
    draw_header(header);

    std::vector<BookPeriodEntry> books = db_get_books_in_period(start, end);

    long total_seconds = 0;
    for (const auto &b : books) total_seconds += b.total_seconds;

    // Centered summary
    int y = S(80);
    int cx = ScreenWidth() / 2;
    SetFont(g_font_huge, BLACK);
    std::string total_str = format_hms(total_seconds);
    int tw = StringWidth(total_str.c_str());
    DrawString(cx - tw / 2, y, total_str.c_str());
    y += S(70);
    
    SetFont(g_font_title, BLACK);
    const char *lbl_main = "Total Hours";
    int lw_main = StringWidth(lbl_main);
    DrawString(cx - lw_main / 2, y, lbl_main);
    y += S(30);

    SetFont(g_font_body, DGRAY);
    char bcount[32];
    snprintf(bcount, sizeof(bcount), "%d book%s read", (int)books.size(), books.size() == 1 ? "" : "s");
    int bw = StringWidth(bcount);
    DrawString(cx - bw / 2, y, bcount);
    y += S(40);
    
    draw_divider(y);
    y += S(24);

    int cover_w = S(60);
    int cover_h = S(85);
    int text_x = S(32) + cover_w + S(16);
    int max_tw = ScreenWidth() - text_x - S(32);

    for (size_t i = 0; i < books.size(); i++) {
        const BookPeriodEntry &b = books[i];

        if (y + cover_h + S(16) > ScreenHeight() - S(40)) {
            char more[64];
            snprintf(more, sizeof(more), "+ %d more", (int)(books.size() - i));
            SetFont(g_font_body, DGRAY);
            int mw = StringWidth(more);
            DrawString(cx - mw / 2, y + S(10), more);
            break;
        }

        ibitmap *cover = metadata_cover(b.path, cover_w, cover_h);
        if (cover) {
            DrawBitmap(S(32), y, cover);
        } else {
            FillArea(S(32), y, cover_w, cover_h, LGRAY);
            DrawRect(S(32), y, cover_w, cover_h, DGRAY);
        }

        SetFont(g_font_body, BLACK);
        std::string title = b.title.empty() ? "(untitled)" : b.title;
        // Truncate title
        while (title.length() > 3 && StringWidth(title.c_str()) > max_tw) {
            title = title.substr(0, title.length() - 4) + "...";
        }
        DrawString(text_x, y + S(4), title.c_str());

        SetFont(g_font_small, DGRAY);
        char line[192];
        std::string author = b.author.empty() ? "" : b.author;
        // Truncate author
        while (author.length() > 3 && StringWidth(author.c_str()) > max_tw) {
            author = author.substr(0, author.length() - 4) + "...";
        }
        if (!author.empty()) {
            DrawString(text_x, y + S(28), author.c_str());
        }

        if (b.finished) {
            snprintf(line, sizeof(line), "Finished  ·  %d sess", b.session_count);
        } else {
            snprintf(line, sizeof(line), "%d%% read  ·  %d sess", (int)(b.progress * 100), b.session_count);
        }
        DrawString(text_x, y + S(52), line);

        // Progress bar
        int bar_y = y + S(74);
        int bar_w = max_tw;
        int bar_h = S(6);
        float progress = b.progress;
        
        DrawRect(text_x, bar_y, bar_w, bar_h, DGRAY);
        FillArea(text_x + 1, bar_y + 1, (int)((bar_w - 2) * progress), bar_h - 2, BLACK);

        y += cover_h + S(24);
        
        if (i < books.size() - 1) {
            draw_divider(y - S(12));
        }
    }

    if (books.empty()) {
        SetFont(g_font_body, DGRAY);
        const char *msg = "No books logged in this period.";
        int mw = StringWidth(msg);
        DrawString(cx - mw / 2, y, msg);
    }
}

static void draw_calendar() {
    time_t now = time(NULL);
    struct tm *lt = localtime(&now);
    int cur_year = lt->tm_year + 1900;
    int cur_month = lt->tm_mon + 1;
    int today_day = lt->tm_mday;

    char header[32];
    snprintf(header, sizeof(header), "%s %d", MONTH_NAME(cur_month), cur_year);
    draw_header(header);

    StreakInfo streak = db_get_streaks();
    int cx = ScreenWidth() / 2;
    int y = S(60);

    // Centered streak display
    char streak_val[16];
    snprintf(streak_val, sizeof(streak_val), "%d", streak.current_streak);
    SetFont(g_font_huge, BLACK);
    int sw = StringWidth(streak_val);
    DrawString(cx - sw / 2, y, streak_val);
    y += S(70);
    
    SetFont(g_font_title, BLACK);
    const char *sl = "Day Streak";
    int slw = StringWidth(sl);
    DrawString(cx - slw / 2, y, sl);
    y += S(30);
    
    draw_divider(y);
    y += S(14);

    time_t m_start = month_start(cur_year, cur_month);
    time_t m_end = (cur_month == 12) ? month_start(cur_year + 1, 1) : month_start(cur_year, cur_month + 1);
    std::vector<DayStat> days = db_get_daily_totals_in_period(m_start, m_end);

    long seconds_by_day[32] = {0};
    for (const auto &d : days) {
        if (d.day >= 1 && d.day <= 31) seconds_by_day[d.day] = d.total_seconds;
    }

    struct tm first = {};
    first.tm_year = cur_year - 1900;
    first.tm_mon = cur_month - 1;
    first.tm_mday = 1;
    mktime(&first);
    int start_weekday = first.tm_wday;

    int days_in_month = 31;
    {
        struct tm probe = first;
        probe.tm_mday = 32;
        mktime(&probe);
        if (probe.tm_mon != first.tm_mon) days_in_month = 32 - probe.tm_mday;
    }

    int margin = S(20);
    int grid_w = ScreenWidth() - margin * 2;
    int cell_w = grid_w / 7;
    int cell_h = S(38);

    static const char *WD[] = {"S","M","T","W","T","F","S"};
    SetFont(g_font_small, DGRAY);
    for (int i = 0; i < 7; i++) {
        int ww = StringWidth(WD[i]);
        DrawString(margin + i * cell_w + (cell_w - ww) / 2, y, WD[i]);
    }

    int y0 = y + S(22);
    for (int day = 1; day <= days_in_month; day++) {
        int slot = start_weekday + (day - 1);
        int col = slot % 7;
        int row = slot / 7;
        int dx = margin + col * cell_w;
        int dy = y0 + row * cell_h;

        long secs = seconds_by_day[day];
        int fill = WHITE;
        int text_color = BLACK;
        if (secs > 0 && secs < 30 * 60)             { fill = LGRAY; text_color = BLACK; }
        else if (secs >= 30 * 60 && secs < 2 * 3600) { fill = DGRAY; text_color = WHITE; }
        else if (secs >= 2 * 3600)                   { fill = BLACK; text_color = WHITE; }

        if (fill != WHITE) FillArea(dx + 2, dy + 2, cell_w - 4, cell_h - 4, fill);
        DrawRect(dx + 2, dy + 2, cell_w - 4, cell_h - 4, DGRAY);
        if (day == today_day) {
            DrawRect(dx + 1, dy + 1, cell_w - 2, cell_h - 2, BLACK);
            DrawRect(dx + 3, dy + 3, cell_w - 6, cell_h - 6, BLACK);
        }

        char num[4];
        snprintf(num, sizeof(num), "%d", day);
        SetFont(g_font_small, text_color);
        int nw = StringWidth(num);
        DrawString(dx + (cell_w - nw) / 2, dy + S(10), num);
    }

    SetFont(g_font_small, DGRAY);
    const char *legend = "Darker = more time read";
    int lw = StringWidth(legend);
    DrawString(cx - lw / 2, ScreenHeight() - S(42), legend);
}



static void draw_dashboard() {
    ClearScreen();
    if (g_pages.empty()) rebuild_pages();

    const PageDesc &p = g_pages[g_page_index];
    if (p.kind == PAGE_OVERVIEW) {
        draw_overview();
    } else if (p.kind == PAGE_CALENDAR) {
        draw_calendar();
    } else if (p.kind == PAGE_MONTH) {
        time_t start = month_start(p.year, p.month);
        time_t end = (p.month == 12) ? month_start(p.year + 1, 1) : month_start(p.year, p.month + 1);
        draw_period_detail(p.label, start, end);
    } else { // PAGE_YEAR
        time_t start = year_start(p.year);
        time_t end = year_start(p.year + 1);
        draw_period_detail(p.label, start, end);
    }

    draw_footer_pageinfo();
    FullUpdate();
}

static int get_daemon_pid() {
    FILE *f = fopen("/tmp/pbreadstats.pid", "r");
    if (!f) return 0;
    int pid = 0;
    if (fscanf(f, "%d", &pid) != 1) {
        fclose(f);
        return 0;
    }
    fclose(f);

    char path[64];
    snprintf(path, sizeof(path), "/proc/%d", pid);
    FILE *proc = fopen(path, "r");
    if (proc) {
        fclose(proc);
        return pid;
    }
    return 0;
}

static std::string get_executable_path() {
    char buf[1024];
    ssize_t len = readlink("/proc/self/exe", buf, sizeof(buf) - 1);
    if (len != -1) {
        buf[len] = '\0';
        return std::string(buf);
    }
    return "";
}

static void start_daemon() {
    if (get_daemon_pid() > 0) {
        db_log("start_daemon: Daemon is already running.");
        return;
    }
    std::string exe_path = get_executable_path();
    if (exe_path.empty()) {
        db_log("start_daemon: Failed to resolve executable path.");
        return;
    }
    std::string cmd = "\"" + exe_path + "\" --daemon &";
    db_log("start_daemon: Launching daemon command: %s", cmd.c_str());
    system(cmd.c_str());
}

static void sigterm_handler(int signum) {
    db_log("sigterm_handler: Received signal %d. Flushing tracker...", signum);
    tracker_flush();
    db_close();
    db_log("sigterm_handler: DB closed. Exiting daemon.");
    exit(0);
}

static void sigusr1_handler(int signum) {
    db_log("sigusr1_handler: Received signal %d. Forcing tracker flush...", signum);
    tracker_flush();
}

static void run_daemon() {
    g_daemon_mode = true;
    db_log("run_daemon: Daemon process started (PID: %d)", getpid());

    signal(SIGTERM, sigterm_handler);
    signal(SIGINT, sigterm_handler);
    signal(SIGUSR1, sigusr1_handler);

    FILE *f = fopen("/tmp/pbreadstats.pid", "w");
    if (f) {
        fprintf(f, "%d\n", getpid());
        fclose(f);
    }

    if (!db_open(DB_PATH)) {
        db_log("run_daemon: Failed to open DB. Exiting daemon.");
        return;
    }

    db_log("run_daemon: DB opened successfully. Starting poll loop...");

    tracker_poll();

    while (true) {
        sleep(15);
        tracker_poll();
    }
}

static int main_handler(int type, int par1, int par2) {
    switch (type) {
        case EVT_INIT:
            db_log("EVT_INIT: Starting application initialization.");
            {
                g_scale = ScreenWidth() / 600.0;
                if (g_scale < 1.0) g_scale = 1.0;
                int size_title = (int)(26 * g_scale);
                int size_body  = (int)(20 * g_scale);
                int size_small = (int)(15 * g_scale);
                int size_huge  = (int)(64 * g_scale);
                g_font_title = OpenFont(DEFAULTFONTB, size_title, 1);
                g_font_body  = OpenFont(DEFAULTFONT, size_body, 1);
                g_font_small = OpenFont(DEFAULTFONT, size_small, 1);
                g_font_huge  = OpenFont(DEFAULTFONTB, size_huge, 1);
            }
            iv_buildpath(DB_PATH);
            if (!db_open(DB_PATH)) {
                db_log("EVT_INIT: db_open failed for path: %s", DB_PATH);
                Message(ICON_ERROR, "Database Error", "Failed to open or initialize the database reading_stats.db.", 5000);
                CloseApp();
                return 1;
            }
            db_log("EVT_INIT: db_open succeeded. Checking daemon...");
            start_daemon();
            return 1;

        case EVT_SHOW:
            db_log("EVT_SHOW: Rendering dashboard.");
            {
                int pid = get_daemon_pid();
                if (pid > 0) {
                    db_log("EVT_SHOW: Sending SIGUSR1 to daemon (PID %d) to flush session.", pid);
                    kill(pid, SIGUSR1);
                    usleep(150000); // Give daemon 150ms to flush DB
                }
            }
            rebuild_pages();
            draw_dashboard();
            return 1;

        case EVT_POINTERUP: {
            // Standard inkview convention: par1/par2 are the x/y
            // coordinates of the pointer event. Left third goes
            // back a page, right third goes forward; the middle is
            // left free for a future "refresh" tap.
            int x = par1;
            int third = ScreenWidth() / 3;
            if (x < third && g_page_index > 0) {
                g_page_index--;
                draw_dashboard();
            } else if (x > third * 2 && g_page_index < (int)g_pages.size() - 1) {
                g_page_index++;
                draw_dashboard();
            }
            return 1;
        }

        case EVT_KEYPRESS:
            if (par1 == KEY_BACK) {
                CloseApp();
            } else if (par1 == KEY_RIGHT && g_page_index < (int)g_pages.size() - 1) {
                g_page_index++;
                draw_dashboard();
            } else if (par1 == KEY_LEFT && g_page_index > 0) {
                g_page_index--;
                draw_dashboard();
            }
            return 1;

        case EVT_EXIT:
            db_log("EVT_EXIT: Closing GUI app.");
            db_close();
            if (g_font_title) CloseFont(g_font_title);
            if (g_font_body) CloseFont(g_font_body);
            if (g_font_small) CloseFont(g_font_small);
            return 1;

        default:
            return 0;
    }
}

int main(int argc, char **argv) {
    if (argc > 1 && strcmp(argv[1], "--daemon") == 0) {
        run_daemon();
        return 0;
    }

    InkViewMain(main_handler);
    return 0;
}

extern "C" {
    char* secure_getenv(const char* name) {
        return getenv(name);
    }
    int getentropy(void *buffer, size_t length) {
        return -1;
    }
}
