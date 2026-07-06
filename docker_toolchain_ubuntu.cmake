set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR arm)

set(SDK_SYSROOT "/sdk/FRSCSDK/arm-none-linux-gnueabi/sysroot")

set(CMAKE_C_COMPILER "/usr/bin/arm-linux-gnueabi-gcc")
set(CMAKE_CXX_COMPILER "/usr/bin/arm-linux-gnueabi-g++")

set(CMAKE_FIND_ROOT_PATH "${SDK_SYSROOT}")
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)

set(CMAKE_C_FLAGS "--sysroot=${SDK_SYSROOT}" CACHE STRING "" FORCE)
set(CMAKE_CXX_FLAGS "--sysroot=${SDK_SYSROOT}" CACHE STRING "" FORCE)
set(CMAKE_EXE_LINKER_FLAGS "--sysroot=${SDK_SYSROOT} -static-libstdc++ -static-libgcc" CACHE STRING "" FORCE)
