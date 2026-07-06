# pb-reading-tracker

A beautiful, native PocketBook application that gives you Kobo-style reading stats: total time read, per-book totals, session counts, and reading streaks.

It combines intelligent background session tracking with native device databases to provide **100% accurate** reading statistics.

## Features

- **Accurate "Books Finished" Counter**: Integrates directly with the PocketBook `explorer-3.db` to precisely track when you've reached the very last page of a book.
- **Real Progress Bars**: The visual progress bars reflect your exact page progress in the book.
- **Reading Streak Calendar**: A KOReader-style heatmap for the current month: one cell per day, shaded by how much you read that day.
- **Smart Tracking**: Polls the device's native `lastopen.txt` (or `/tmp/.current`) to detect exactly when a book is opened and closed, accurately logging your reading sessions.
- **Kobo "Reading Life" UI**: Features premium, large typography with crisp, readable stats and beautifully aligned book thumbnails.

## Dashboard Pages

Tap the left/right edge of the screen (or use physical left/right keys) to move through the stats dashboard:

- **Overview** — All-time totals, current & best streaks, and a visual list of your most recently opened books with accurate reading percentages.
- **Calendar** — A heatmap of your daily reading habits for the current month.
- **Monthly & Yearly Summaries** — Detailed lists of exactly which books you read in that period, complete with session counts, covers, and completion status.

## Installation (For Regular Users)

The easiest way to use this app is to install the pre-compiled version:

1. Download the `Reading Stats.app` file from the `release/` folder in this repository.
2. Connect your PocketBook to your computer via USB.
3. Copy the `Reading Stats.app` file into your PocketBook's `applications/` folder.
4. Eject the device, and the app will appear in your Applications menu!

*(Optional: If you want a custom icon, place a 114x114 BMP file named `Reading Stats.app.bmp` next to the app in the `applications/` folder).*

## Building from Source (For Developers)

This project is built in C++ using the official PocketBook SDK (inkview). If you want to edit the code and compile it yourself:

```bash
cmake -B build -DCMAKE_TOOLCHAIN_FILE=/path/to/your/pocketbook_toolchain.cmake
cmake --build build
```

This produces a single `pbreadstats` binary. You can rename it to `Reading Stats.app` and deploy it.

## How it works under the hood

1. **Session Detection**: A background timer runs in the app and polls `/mnt/ext1/system/state/lastopen.txt`.
2. **Metadata**: When a session closes, it is written to `/mnt/ext1/system/pbreadstats/reading_stats.db`, along with metadata pulled via `GetBookInfo()`.
3. **Native Progress**: The UI cross-references your session data with the device's native `/mnt/ext1/system/explorer-3/explorer-3.db` database to retrieve exact `cpage` and `npage` values.

## License
MIT License
