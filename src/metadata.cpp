#include "metadata.h"
#include <inkview.h>
#include <cstring>

static std::string safe(const char *s) {
    return s ? std::string(s) : std::string();
}

// Pull a reasonable title out of a bare filename when the library
// has no metadata for a book yet (e.g. it was just copied over and
// hasn't been indexed, or GetBookInfo() otherwise comes back empty).
static std::string title_from_filename(const std::string &path) {
    size_t slash = path.find_last_of('/');
    std::string name = (slash == std::string::npos) ? path : path.substr(slash + 1);
    size_t dot = name.find_last_of('.');
    if (dot != std::string::npos) name = name.substr(0, dot);
    return name;
}

BookMeta metadata_read(const std::string &path) {
    BookMeta m;
    m.path = path;

    bookinfo *bi = GetBookInfo(path.c_str());
    if (bi) {
        m.title  = safe(bi->title);
        m.author = safe(bi->author);
        m.series = safe(bi->series);
        m.year   = bi->year;
        m.size_bytes = bi->size;

        std::string genres;
        for (int i = 0; i < 10; i++) {
            if (!bi->genre[i] || !bi->genre[i][0]) continue;
            if (!genres.empty()) genres += ", ";
            genres += bi->genre[i];
        }
        m.genre = genres;
    }

    if (m.title.empty()) {
        m.title = title_from_filename(path);
    }

    return m;
}

// Lightweight version that avoids InkView calls (GetBookInfo),
// safe to call from the headless daemon process.
BookMeta metadata_read_basic(const std::string &path) {
    BookMeta m;
    m.path = path;
    m.year = 0;
    m.size_bytes = 0;

    // Try to parse "Title - Author" from the filename
    std::string name = title_from_filename(path);

    // Many ebook filenames use " - " to separate title and author
    size_t sep = name.find(" - ");
    if (sep != std::string::npos) {
        m.title  = name.substr(0, sep);
        m.author = name.substr(sep + 3);
    } else {
        m.title = name;
    }

    return m;
}

ibitmap *metadata_cover(const std::string &path, int w, int h) {
    // GetBookCover returns a library-owned/cached bitmap (same
    // convention as GetBookInfo's bookinfo*) -- we don't free it.
    // It can legitimately return NULL for books the library
    // hasn't indexed a cover for yet.
    return GetBookCover(path.c_str(), w, h);
}
