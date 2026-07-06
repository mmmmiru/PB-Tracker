cmake_minimum_required(VERSION 3.10)
project(pbreadstats CXX)

set(CMAKE_CXX_STANDARD 14)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Point this at your PocketBook SDK toolchain file, e.g.:
#   cmake -B build -DCMAKE_TOOLCHAIN_FILE=/path/to/arm-obreey-linux-gnueabi.cmake
# (Same toolchain you're already using for pb-stats-tracker.)

add_executable(pbreadstats
    src/main.cpp
    src/tracker.cpp
    src/db.cpp
    src/metadata.cpp
)

target_link_libraries(pbreadstats inkview sqlite3 z)
