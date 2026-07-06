set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR arm)

set(SDK_SYSROOT "/Users/mirandadiazhernandez/pocketbook-sdk/FRSCSDK/arm-none-linux-gnueabi/sysroot")

set(CMAKE_C_COMPILER "/opt/homebrew/bin/arm-linux-gnueabi-gcc")
set(CMAKE_CXX_COMPILER "/opt/homebrew/bin/arm-linux-gnueabi-g++")

set(CMAKE_FIND_ROOT_PATH "${SDK_SYSROOT}")
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)

set(CMAKE_C_FLAGS "--sysroot=${SDK_SYSROOT}" CACHE STRING "" FORCE)
set(CMAKE_CXX_FLAGS "--sysroot=${SDK_SYSROOT} -include /Users/mirandadiazhernandez/Downloads/pb-reading-tracker/stdlib_fix.h" CACHE STRING "" FORCE)
set(CMAKE_EXE_LINKER_FLAGS "--sysroot=${SDK_SYSROOT} -static-libstdc++ -static-libgcc" CACHE STRING "" FORCE)
