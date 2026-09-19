# ---------------------------------------------------------------------------
# arm-none-eabi.cmake -- CMake toolchain file for bare-metal Cortex-M0+.
#
# WHAT A TOOLCHAIN FILE IS: CMake normally assumes it is building for the
# machine it is running on. It tests the compiler by building and RUNNING a
# small program. That obviously cannot work when the target is a chip on your
# desk. A toolchain file is read BEFORE any of that happens and tells CMake
# "you are cross-compiling, here is the compiler, do not try to run anything".
#
# It is passed on the very first configure and then remembered:
#     cmake -B build -DCMAKE_TOOLCHAIN_FILE=cmake/arm-none-eabi.cmake
#
# This file is generic Cortex-M0+ boilerplate -- nothing in it is specific to
# your project. The MSPM0/TI-specific parts all live in CMakeLists.txt.
# ---------------------------------------------------------------------------

# "Generic" is CMake's name for "no operating system". This single line is what
# switches CMake into cross-compiling mode.
set(CMAKE_SYSTEM_NAME      Generic)
set(CMAKE_SYSTEM_PROCESSOR arm)

# --- THE LINE THAT SAVES YOU AN HOUR ---------------------------------------
# By default CMake validates the compiler by building a complete EXECUTABLE.
# Linking a bare-metal executable needs a linker script and a startup file,
# which CMake does not have during its own test -- so the link fails and CMake
# declares "the C compiler is not able to compile a simple test program",
# which sounds like your toolchain is broken when it is perfectly fine.
#
# Telling it to build a STATIC LIBRARY for the test means it compiles but never
# links, so the check passes. This is the single most common wall people hit
# when moving an embedded project to CMake.
set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)

# --- the tools -------------------------------------------------------------
# These are found on PATH (brew put symlinks in /opt/homebrew/bin). If you ever
# need to pin an exact toolchain version, replace these with absolute paths to
# /Applications/ArmGNUToolchain/<version>/arm-none-eabi/bin/...
set(TC arm-none-eabi-)
set(CMAKE_C_COMPILER   ${TC}gcc)
set(CMAKE_ASM_COMPILER ${TC}gcc)   # gcc drives the assembler for .S files
set(CMAKE_OBJCOPY      ${TC}objcopy CACHE FILEPATH "objcopy")
set(CMAKE_SIZE         ${TC}size    CACHE FILEPATH "size")

# --- what CPU we are building for ------------------------------------------
#   -mcpu=cortex-m0plus   the specific core
#   -mthumb               ARMv6-M only has the Thumb instruction set
#   -march=armv6-m        the architecture level (no SMULL, no SDIV, no DSP)
#   -mfloat-abi=soft      no FPU on this part: floats become library calls
# These must be identical for COMPILING and LINKING, or the linker will pick
# libraries built for the wrong architecture and fail in confusing ways.
set(CPU_FLAGS "-mcpu=cortex-m0plus -mthumb -march=armv6-m -mfloat-abi=soft")

# -ffunction-sections/-fdata-sections put every function and variable in its own
# section so the linker's --gc-sections can discard the ones nothing references.
# On a 128 KB part that is free size savings.
set(CMAKE_C_FLAGS_INIT   "${CPU_FLAGS} -ffunction-sections -fdata-sections")
set(CMAKE_ASM_FLAGS_INIT "${CPU_FLAGS}")

# --specs=nano.specs  = newlib-nano, a libc that does not assume an OS or a
#                       large heap. Much smaller than full newlib.
# --specs=nosys.specs = stubs for the syscalls newlib expects (_write, _sbrk...)
#                       so the link succeeds. printf-to-nowhere until you
#                       retarget _write to the UART.
set(CMAKE_EXE_LINKER_FLAGS_INIT "${CPU_FLAGS} --specs=nano.specs --specs=nosys.specs")

# Only look for headers and libraries inside the toolchain's own sysroot, never
# in /usr/include or /opt/homebrew. Without this CMake can happily hand you a
# macOS x86/arm64 header and you will not find out until link time.
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM BEFORE)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)
