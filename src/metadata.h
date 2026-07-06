#ifndef PBRS_METADATA_H
#define PBRS_METADATA_H

#include <string>
#include <inkview.h> // for ibitmap

// Plain-old-data snapshot of everything we can pull about a book
// via inkview's GetBookInfo(), sanitized to always contain valid
// (possibly empty) strings so callers never have to null-check.
struct BookMeta {
    std::string path;
    std::string title;
    std::string author;
    std::string series;
    std::string genre;   // comma-joined, since bookinfo has up to 10 slots
    int year;
    long size_bytes = 0;
};

// Reads metadata for the book at `path` using the inkview library.
// Falls back to deriving a title from the filename if the library
// can't produce one (e.g. format it doesn't index well, or a book
// that was never added to the library database yet).
BookMeta metadata_read(const std::string &path);

// Lightweight version that only parses the filename for title/author.
// Safe to call from the headless daemon process (no InkView calls).
BookMeta metadata_read_basic(const std::string &path);

// Fetches a cover thumbnail sized to (w, h) via inkview's
// GetBookCover(). Returns NULL if the book has no extractable
// cover (never opened yet, unsupported format, etc.) -- callers
// should draw a placeholder in that case rather than skip the row,
// so the list stays visually aligned.
ibitmap *metadata_cover(const std::string &path, int w, int h);

#endif
