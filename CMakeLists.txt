# ---------------------------------------------------------------------------
# Tactic -- MSPM0G3507 (Cortex-M0+) firmware build.
#
# Configure once, then build as often as you like:
#     cmake -B build -DCMAKE_TOOLCHAIN_FILE=cmake/arm-none-eabi.cmake
#     cmake --build build
#
# The toolchain file (cmake/arm-none-eabi.cmake) handles everything generic to
# bare-metal Cortex-M. Everything TI- and board-specific is in THIS file.
# ---------------------------------------------------------------------------
cmake_minimum_required(VERSION 3.20)

# ASM is listed because the startup path can pull in assembly. Listing a
# language you end up not using costs nothing; omitting one you do use gives a
# baffling error, so it is the cheaper default.
project(tactic C ASM)

# With a single-config generator (Make, Ninja) CMAKE_BUILD_TYPE defaults to
# EMPTY -- which means no -O flag at all, i.e. -O0. That is not a warning, it
# just silently produces a bigger, slower binary. Caught here by the build
# coming out at 1176 B when TI's -O2 makefile produced 688 B.
# Default to Release so the size numbers you quote are the real ones.
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
    message(STATUS "CMAKE_BUILD_TYPE was empty; defaulting to Release")
endif()

# Bare-metal ELF. DSLite does not care about the extension, but .elf is what
# the rest of the world calls this file, and .out is a TI-ism.
set(CMAKE_EXECUTABLE_SUFFIX ".elf")

# --- where the SDK lives ---------------------------------------------------
# A cache variable, so it can be overridden on another machine without editing
# this file:  cmake -B build -DMSPM0_SDK=/somewhere/else ...
set(MSPM0_SDK "$ENV{HOME}/ti/mspm0_sdk_2_10_00_04"
    CACHE PATH "Path to the TI MSPM0 SDK")

if(NOT EXISTS "${MSPM0_SDK}/source/ti/driverlib")
    message(FATAL_ERROR
        "MSPM0 SDK not found at ${MSPM0_SDK}\n"
        "Pass -DMSPM0_SDK=/path/to/mspm0_sdk_X_XX_XX_XX")
endif()

# Prebuilt driverlib for the G1x0x/G3x0x family, built for GCC / Cortex-M0+.
# TI ships this compiled, so there is nothing to build from source.
set(DRIVERLIB "${MSPM0_SDK}/source/ti/driverlib/lib/gcc/m0p/mspm0g1x0x_g3x0x/driverlib.a")
if(NOT EXISTS "${DRIVERLIB}")
    message(FATAL_ERROR "driverlib.a not found at ${DRIVERLIB}")
endif()

# The linker script: the memory map. It is what tells the linker that flash
# starts at 0x0 and is 128 KB, SRAM starts at 0x20200000 and is 32 KB, and
# where each section (.text, .data, .bss, the stack) goes. Copied into vendor/
# from the SDK so this repo builds on a machine with no CCS.
set(LINKER_SCRIPT "${CMAKE_SOURCE_DIR}/vendor/device_linker.lds")

# --- sources ---------------------------------------------------------------
# src/    = your code.        vendor/ = unmodified TI files.
# Listed explicitly rather than with GLOB: a glob does not re-run when you add
# a file, so new sources silently fail to build until you reconfigure.

# The runtime entry point is the last piece of this project. Until src/main.c
# exists there is nothing to link, so say that plainly instead of failing the
# configure step with "Cannot find source file". See src/RUNTIME_PLAN.md.
if(NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/src/main.c")
    message(STATUS
        "src/main.c is not in the tree yet -- skipping the firmware target. "
        "Host tools are unaffected:  cd host && make && ./btnfit")
    return()
endif()

add_executable(${PROJECT_NAME}
    src/main.c

    vendor/startup_mspm0g350x_gcc.c   # vector table, Reset_Handler: zeroes .bss,
                                      # copies .data from flash to SRAM, calls main
    vendor/ti_msp_dl_config.c         # SysConfig output: pin mux, clocks, peripherals
)

target_include_directories(${PROJECT_NAME} PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/vendor                              # ti_msp_dl_config.h
    ${MSPM0_SDK}/source                                     # ti/driverlib/...
    ${MSPM0_SDK}/source/third_party/CMSIS/Core/Include       # core_cm0plus.h
)

# These two come from the SysConfig-generated device.opt in the TI build.
#   __MSPM0G3507__    selects the device header and its register map
#   __USE_SYSCONFIG__ tells driverlib the config comes from ti_msp_dl_config
target_compile_definitions(${PROJECT_NAME} PRIVATE
    __MSPM0G3507__
    __USE_SYSCONFIG__
)

target_compile_options(${PROJECT_NAME} PRIVATE
    -std=c99
    -Wall -Wextra
    # -Wconversion is deliberately NOT on yet -- TI's vendor sources will not
    # survive it. Turn it on per-target for src/ once your fixed-point code
    # lands; it is the single best catch for silent narrowing in Q-format math.
    $<$<CONFIG:Debug>:-Og -g3>
    $<$<CONFIG:Release>:-O2 -g>
)

target_link_options(${PROJECT_NAME} PRIVATE
    -T${LINKER_SCRIPT}                        # use OUR memory map
    -nostartfiles                             # do NOT use the toolchain's crt0;
                                              # vendor/startup_*.c provides it
    -Wl,--gc-sections                         # drop unreferenced code/data
    -Wl,-Map=${PROJECT_NAME}.map              # per-symbol memory report
    -Wl,--print-memory-usage                  # flash/SRAM summary at link time
    -static
)

# driverlib.a is a plain archive, not a CMake target, so it is linked by path.
target_link_libraries(${PROJECT_NAME} PRIVATE ${DRIVERLIB} c m gcc)

# Rebuild if the linker script changes -- CMake cannot infer this dependency
# from a link flag on its own, and a stale memory map is a miserable bug.
set_target_properties(${PROJECT_NAME} PROPERTIES LINK_DEPENDS ${LINKER_SCRIPT})

# --- after every build ------------------------------------------------------
# Print the size and emit .bin/.hex. The size line is where two of your four
# numbers come from, so having it on every build means you never have to go
# measure them separately:
#     flash = text + data        SRAM = data + bss  (+ stack, not shown here)
add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E echo ""
    COMMAND ${CMAKE_SIZE} $<TARGET_FILE:${PROJECT_NAME}>
    COMMAND ${CMAKE_OBJCOPY} -O binary $<TARGET_FILE:${PROJECT_NAME}> ${PROJECT_NAME}.bin
    COMMAND ${CMAKE_OBJCOPY} -O ihex   $<TARGET_FILE:${PROJECT_NAME}> ${PROJECT_NAME}.hex
    COMMENT "size / bin / hex"
)

# --- convenience: cmake --build build --target flash ------------------------
add_custom_target(flash
    COMMAND ${CMAKE_SOURCE_DIR}/tools/flash.sh $<TARGET_FILE:${PROJECT_NAME}>
    DEPENDS ${PROJECT_NAME}
    USES_TERMINAL
    COMMENT "Flashing ${PROJECT_NAME}.elf via tools/flash.sh"
)
