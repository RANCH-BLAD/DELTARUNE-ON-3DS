cmake_minimum_required(VERSION 3.21)
project(butterscotch C)

set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)
add_compile_options(-Wall -Wextra)

set(PLATFORM "" CACHE STRING "Platform backend")

# Build metadata
set(BUTTERSCOTCH_COMMIT_HASH "unknown" CACHE STRING "Git commit hash")
set(BUTTERSCOTCH_COMMIT_DATE "unknown" CACHE STRING "Git commit date")
add_compile_definitions(
    BUTTERSCOTCH_COMMIT_HASH="${BUTTERSCOTCH_COMMIT_HASH}"
    BUTTERSCOTCH_COMMIT_DATE="${BUTTERSCOTCH_COMMIT_DATE}"
)

if(PLATFORM STREQUAL "")
    message(FATAL_ERROR "PLATFORM is not set! Use -DPLATFORM=glfw (or another supported platform).")
endif()

# To avoid unused code in production builds that are only used for a single game (example: PlayStation 2 target), we can enable/disable specific bytecode versions
#
# If only a single bytecode version is enabled, the checks will be collapsed and removed by the compiler during build time, reducing code size and improving
# performance on low end hardware!
option(ENABLE_BC16 "Enable support for Bytecode Version 16" ON)
option(ENABLE_BC17 "Enable support for Bytecode Version 17" ON)
if(PLATFORM STREQUAL "n3ds")
    set(ENABLE_BC16 ON CACHE BOOL "Enable support for Bytecode Version 16" FORCE)
    set(ENABLE_BC17 ON CACHE BOOL "Enable support for Bytecode Version 17" FORCE)
endif()
if(NOT ENABLE_BC16 AND NOT ENABLE_BC17)
    message(FATAL_ERROR "You need to build Butterscotch with at least one bytecode version enabled!")
endif()
if(ENABLE_BC16)
    add_compile_definitions(ENABLE_BC16)
endif()
if(ENABLE_BC17)
    add_compile_definitions(ENABLE_BC17)
endif()

set(BUTTERSCOTCH_WIIU_ICON "${CMAKE_SOURCE_DIR}/resources/wiiu/meta/icon.png")
set(BUTTERSCOTCH_WIIU_DRC_SPLASH "${CMAKE_SOURCE_DIR}/resources/wiiu/meta/drc.png")
set(BUTTERSCOTCH_WIIU_TV_SPLASH "${CMAKE_SOURCE_DIR}/resources/wiiu/meta/tv.png")
set(BUTTERSCOTCH_WIIU_NAME "UNDERTALE")
set(BUTTERSCOTCH_WIIU_SHORTNAME "UNDERTALE")
set(BUTTERSCOTCH_WIIU_APP_FOLDER "cinnamon")
set(BUTTERSCOTCH_3DS_NAME "Project Papyrus")
set(BUTTERSCOTCH_3DS_DESCRIPTION "DELTARUNE Ch1 for old 3DS")
set(BUTTERSCOTCH_3DS_AUTHOR "Project Sunshine")
if(ENABLE_BC17 AND NOT ENABLE_BC16)
    set(BUTTERSCOTCH_WIIU_ICON "${CMAKE_SOURCE_DIR}/resources/wiiu/meta/icon-deltarune.png")
    set(BUTTERSCOTCH_WIIU_DRC_SPLASH "${CMAKE_SOURCE_DIR}/resources/wiiu/meta/drc-deltarune.png")
    set(BUTTERSCOTCH_WIIU_TV_SPLASH "${CMAKE_SOURCE_DIR}/resources/wiiu/meta/tv-deltarune.png")
    set(BUTTERSCOTCH_WIIU_NAME "DELTARUNE")
    set(BUTTERSCOTCH_WIIU_SHORTNAME "DELTARUNE")
endif()
option(WIIU_USE_DELTARUNE_SD_PATH "Use /wiiu/apps/deltarune instead of /wiiu/apps/cinnamon on Wii U" OFF)
if(WIIU_USE_DELTARUNE_SD_PATH)
    set(BUTTERSCOTCH_WIIU_APP_FOLDER "deltarune")
endif()

file(GLOB SOURCES src/*.c)
# Platform specific files builds
file(GLOB PLATFORM_SOURCES src/${PLATFORM}/*.c)

# OpenGL ES 3.0 / WebGL2 compatibility
option(ENABLE_GLES "Build the gl_renderer against OpenGL ES 3.0 / WebGL2 instead of desktop GL 4.1 Core" OFF)
if(PLATFORM STREQUAL "glfw" AND ENABLE_GLES)
    list(REMOVE_ITEM PLATFORM_SOURCES ${CMAKE_SOURCE_DIR}/src/glfw/gl_legacy_renderer.c)
endif()

add_executable(butterscotch ${SOURCES} ${PLATFORM_SOURCES})
target_include_directories(butterscotch PRIVATE ${CMAKE_SOURCE_DIR}/src)

# stb_ds
target_include_directories(butterscotch PUBLIC vendor/stb/ds)

if(PLATFORM STREQUAL "glfw")
    file(GLOB GL_SOURCES src/gl/*.c)
    list(REMOVE_ITEM GL_SOURCES ${CMAKE_SOURCE_DIR}/src/gl/gl_renderer.c)
    target_sources(butterscotch PRIVATE ${GL_SOURCES})
    target_include_directories(butterscotch PRIVATE ${CMAKE_SOURCE_DIR}/src/gl)
    target_include_directories(butterscotch PRIVATE ${CMAKE_SOURCE_DIR}/vendor)

    # Butterscotch VM/interpreter profiler
    option(ENABLE_VM_GML_PROFILER "Enable Butterscotch VM/interpreter profiler" ON)
    if(ENABLE_VM_GML_PROFILER)
        add_compile_definitions(ENABLE_VM_GML_PROFILER)
    endif()

    # Butterscotch VM/interpreter tracing
    option(ENABLE_VM_TRACING "Enable Butterscotch VM/interpreter tracing checks" ON)
    if(ENABLE_VM_TRACING)
        add_compile_definitions(ENABLE_VM_TRACING)
    endif()

    # Butterscotch VM opcode count profiler
    option(ENABLE_VM_OPCODE_PROFILER "Enable Butterscotch VM opcode count profiler" ON)
    if(ENABLE_VM_OPCODE_PROFILER)
        add_compile_definitions(ENABLE_VM_OPCODE_PROFILER)
    endif()

    # Spatial grid logs
    option(ENABLE_SPATIAL_GRID_LOGS "Enable Spatial Grid logs" OFF)
    if(ENABLE_SPATIAL_GRID_LOGS)
        add_compile_definitions(ENABLE_SPATIAL_GRID_LOGS)
    endif()

    # VM unknown/stubbed function logs
    option(ENABLE_VM_STUB_LOGS "Enable VM unknown/stubbed function logs" ON)
    if(ENABLE_VM_STUB_LOGS)
        add_compile_definitions(ENABLE_VM_STUB_LOGS)
    endif()

    # GLAD
    if(ENABLE_GLES)
        add_compile_definitions(ENABLE_GLES)
        add_library(glad STATIC vendor/glad-gles/src/glad.c)
        target_include_directories(glad PUBLIC vendor/glad-gles/include)
    else()
        add_library(glad STATIC vendor/glad/src/glad.c)
        target_include_directories(glad PUBLIC vendor/glad/include)
    endif()

    # stb_image
    target_include_directories(butterscotch PUBLIC vendor/stb/image)

    # stb_vorbis
    target_include_directories(butterscotch PUBLIC vendor/stb/vorbis)

    # miniaudio
    target_include_directories(butterscotch PUBLIC vendor/miniaudio)

    if(MINGW)
        find_package(glfw3 REQUIRED)
        set(GLFW3_LIBRARIES glfw3)

        # Target Windows 7+ (avoids referencing newer APIs like MapViewOfFileNuma2 that MinGW import libs don't provide)
        # https://github.com/mirror/mingw-w64/blob/master/mingw-w64-headers/include/sdkddkver.h
        target_compile_definitions(butterscotch PRIVATE _WIN32_WINNT=0x0601 NTDDI_VERSION=0x06010000 WIN32_LEAN_AND_MEAN)
        target_link_libraries(butterscotch ${GLFW3_LIBRARIES} glad m opengl32 gdi32 winmm bz2)
        target_link_options(butterscotch PRIVATE -static)
    elseif(CMAKE_SYSTEM_NAME STREQUAL "Haiku")
        find_package(PkgConfig REQUIRED)
        pkg_check_modules(GLFW3 REQUIRED glfw3)
        target_include_directories(butterscotch PRIVATE ${GLFW3_INCLUDE_DIRS})
        target_link_directories(butterscotch PRIVATE ${GLFW3_LIBRARY_DIRS})
        target_link_libraries(butterscotch ${GLFW3_LIBRARIES} glad m pthread bz2)
    else()
        find_package(PkgConfig REQUIRED)
        pkg_check_modules(GLFW3 REQUIRED glfw3)
        target_include_directories(butterscotch PRIVATE ${GLFW3_INCLUDE_DIRS})
        target_link_directories(butterscotch PRIVATE ${GLFW3_LIBRARY_DIRS})
        target_link_libraries(butterscotch ${GLFW3_LIBRARIES} glad m pthread dl bz2)
    endif()

    # Enable AddressSanitizer by default in Debug builds
    if(CMAKE_BUILD_TYPE STREQUAL "Debug")
        set(ENABLE_ASAN_DEFAULT ON)
    else()
        set(ENABLE_ASAN_DEFAULT OFF)
    endif()
    set(ENABLE_ASAN ${ENABLE_ASAN_DEFAULT} CACHE BOOL "Enable AddressSanitizer")
    if(ENABLE_ASAN AND NOT MINGW AND NOT CMAKE_SYSTEM_NAME STREQUAL "Haiku")
        target_compile_options(butterscotch PRIVATE -fsanitize=address -fno-omit-frame-pointer)
        target_link_options(butterscotch PRIVATE -fsanitize=address)
    endif()

    #gamecontrollerdb
    add_custom_command(TARGET butterscotch POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            ${CMAKE_SOURCE_DIR}/vendor/gamecontrollerdb.txt
            $<TARGET_FILE_DIR:butterscotch>/gamecontrollerdb.txt
    )
elseif(PLATFORM STREQUAL "wiiu")
    add_compile_definitions(USE_FLOAT_REALS)
    add_compile_definitions(NO_RVALUE_INT64)
    # find_package(SDL2 CONFIG REQUIRED)
    target_sources(butterscotch PRIVATE ${CMAKE_SOURCE_DIR}/src/gl/image_decoder.c)
    target_compile_definitions(butterscotch PRIVATE BUTTERSCOTCH_WIIU_APP_FOLDER="${BUTTERSCOTCH_WIIU_APP_FOLDER}")

    if(DEFINED ENV{DEVKITPRO} AND NOT "$ENV{DEVKITPRO}" STREQUAL "")
        file(TO_CMAKE_PATH "$ENV{DEVKITPRO}" DEVKITPRO_ROOT)
    elseif(EXISTS "C:/devkitPro")
        file(TO_CMAKE_PATH "C:/devkitPro" DEVKITPRO_ROOT)
    else()
        message(FATAL_ERROR "DEVKITPRO is not set and C:/devkitPro was not found.")
    endif()

    # data.win and bytecode blobs are serialized little-endian, but Wii U's PowerPC CPU is big-endian.
    target_compile_definitions(butterscotch PRIVATE IS_BIG_ENDIAN)

    target_compile_options(butterscotch PRIVATE
        -O3
        -ffunction-sections -fdata-sections
        -fsingle-precision-constant
        -fomit-frame-pointer
        -ffast-math
    )

    target_include_directories(butterscotch PRIVATE
        ${CMAKE_SOURCE_DIR}/vendor
        ${CMAKE_SOURCE_DIR}/vendor/stb/image
        ${CMAKE_SOURCE_DIR}/vendor/stb/vorbis
        ${CMAKE_SOURCE_DIR}/vendor/miniaudio
        ${DEVKITPRO_ROOT}/wut/include
        ${DEVKITPRO_ROOT}/portlibs/wiiu/include
        ${DEVKITPRO_ROOT}/portlibs/ppc/include
    )

    target_link_directories(butterscotch PRIVATE
        ${DEVKITPRO_ROOT}/portlibs/wiiu/lib
        ${DEVKITPRO_ROOT}/portlibs/ppc/lib
    )
    target_link_libraries(butterscotch PRIVATE bz2 SDL2)

    set_target_properties(butterscotch PROPERTIES OUTPUT_NAME undertale)

    if(NOT COMMAND wut_create_rpx)
        message(FATAL_ERROR "wut_create_rpx is unavailable. Configure with the devkitPro Wii U toolchain file.")
    endif()

    wut_create_rpx(butterscotch)

    wut_create_wuhb(butterscotch
        NAME ${BUTTERSCOTCH_WIIU_NAME}
        SHORTNAME ${BUTTERSCOTCH_WIIU_SHORTNAME}
        AUTHOR "Toby Fox, Project Sunshine"
        DRCSPLASH ${BUTTERSCOTCH_WIIU_DRC_SPLASH}
        ICON ${BUTTERSCOTCH_WIIU_ICON}
        TVSPLASH ${BUTTERSCOTCH_WIIU_TV_SPLASH}
        CONTENT ${CMAKE_SOURCE_DIR}/resources/wiiu/content
    )

elseif(PLATFORM STREQUAL "ps2")
    # The PS2 EE has a single-precision FPU only, so double is software-emulated
    add_compile_definitions(USE_FLOAT_REALS)
    # By disabling Int64 support, we can fit the entire RValue struct in 8 bytes, improving performance and memory usage
    add_compile_definitions(NO_RVALUE_INT64)

    # Butterscotch VM/interpreter profiler
    option(ENABLE_VM_GML_PROFILER "Enable Butterscotch VM/interpreter profiler" OFF)
    if(ENABLE_VM_GML_PROFILER)
        add_compile_definitions(ENABLE_VM_GML_PROFILER)
    endif()

    # Butterscotch VM/interpreter tracing
    option(ENABLE_VM_TRACING "Enable Butterscotch VM/interpreter tracing checks" OFF)
    if(ENABLE_VM_TRACING)
        add_compile_definitions(ENABLE_VM_TRACING)
    endif()

    # Butterscotch VM opcode count profiler
    option(ENABLE_VM_OPCODE_PROFILER "Enable Butterscotch VM opcode count profiler" OFF)
    if(ENABLE_VM_OPCODE_PROFILER)
        add_compile_definitions(ENABLE_VM_OPCODE_PROFILER)
    endif()

    # Spatial grid logs
    option(ENABLE_SPATIAL_GRID_LOGS "Enable Spatial Grid logs" OFF)
    if(ENABLE_SPATIAL_GRID_LOGS)
        add_compile_definitions(ENABLE_SPATIAL_GRID_LOGS)
    endif()

    # VM unknown/stubbed function logs
    option(ENABLE_VM_STUB_LOGS "Enable VM unknown/stubbed function logs" OFF)
    if(ENABLE_VM_STUB_LOGS)
        add_compile_definitions(ENABLE_VM_STUB_LOGS)
    endif()

    # Platform identifier for PS2-specific code paths (e.g., scratchpad allocation)
    add_compile_definitions(PLATFORM_PS2)
    
    # PS2 renderer hot path logs
    option(ENABLE_PS2_RENDERER_LOGS "Enable hot path logs in the PS2 renderer" OFF)
    if(ENABLE_PS2_RENDERER_LOGS)
        add_compile_definitions(ENABLE_PS2_RENDERER_LOGS)
    endif()

    # PS2 audio system
    option(ENABLE_PS2_AUDIO "Enable the PS2 audio system" ON)
    if(ENABLE_PS2_AUDIO)
        add_compile_definitions(ENABLE_PS2_AUDIO)
    else()
        list(REMOVE_ITEM PLATFORM_SOURCES ${CMAKE_SOURCE_DIR}/src/ps2/ps2_audio_system.c)
    endif()

    # -fsingle-precision-constant: Treat unsuffixed float constants as single-precision to avoid accidental  float->double promotions on platforms with no hardware double
    # -Werror=double-promotion: Warn when something is being promoted to a double
    # -fomit-frame-pointer: frees a register on a target with tight register pressure
    # -ffast-math: enables aggressive floating point optimizations
    target_compile_options(butterscotch PRIVATE -fsingle-precision-constant -Werror=double-promotion -fomit-frame-pointer -ffast-math)

    if(PS2)
        # Compiling on the Docker container
        target_include_directories(butterscotch PRIVATE $ENV{PS2SDK}/ee/include $ENV{PS2SDK}/common/include $ENV{PS2DEV}/ee/include $ENV{GSKIT}/include)
        target_link_directories(butterscotch PRIVATE $ENV{PS2SDK}/ee/lib $ENV{GSKIT}/lib)
        target_link_libraries(butterscotch gskit dmakit audsrv pad mc kbd patches kernel c m)

        # Embed ps2sdk IRX modules (freesio2, freepad) as C arrays via bin2c
        # This avoids loading from rom0: which has compatibility issues between fat/slim PS2
        set(IRX_DIR $ENV{PS2SDK}/iop/irx)
        set(IRX_GEN_DIR ${CMAKE_BINARY_DIR}/generated/irx)
        file(MAKE_DIRECTORY ${IRX_GEN_DIR})

        add_custom_command(
            OUTPUT ${IRX_GEN_DIR}/freesio2_irx.c
            COMMAND $ENV{PS2SDK}/bin/bin2c ${IRX_DIR}/freesio2.irx ${IRX_GEN_DIR}/freesio2_irx.c freesio2_irx
            DEPENDS ${IRX_DIR}/freesio2.irx
            COMMENT "Embedding freesio2.irx"
        )
        add_custom_command(
            OUTPUT ${IRX_GEN_DIR}/mcman_irx.c
            COMMAND $ENV{PS2SDK}/bin/bin2c ${IRX_DIR}/mcman.irx ${IRX_GEN_DIR}/mcman_irx.c mcman_irx
            DEPENDS ${IRX_DIR}/mcman.irx
            COMMENT "Embedding mcman.irx"
        )
        add_custom_command(
            OUTPUT ${IRX_GEN_DIR}/mcserv_irx.c
            COMMAND $ENV{PS2SDK}/bin/bin2c ${IRX_DIR}/mcserv.irx ${IRX_GEN_DIR}/mcserv_irx.c mcserv_irx
            DEPENDS ${IRX_DIR}/mcserv.irx
            COMMENT "Embedding mcserv.irx"
        )
        add_custom_command(
            OUTPUT ${IRX_GEN_DIR}/freepad_irx.c
            COMMAND $ENV{PS2SDK}/bin/bin2c ${IRX_DIR}/freepad.irx ${IRX_GEN_DIR}/freepad_irx.c freepad_irx
            DEPENDS ${IRX_DIR}/freepad.irx
            COMMENT "Embedding freepad.irx"
        )
        # We need the USB mass storage IRX modules for writing gmon.out when not running from host: and for keyboard support
        add_custom_command(
            OUTPUT ${IRX_GEN_DIR}/usbd_irx.c
            COMMAND $ENV{PS2SDK}/bin/bin2c ${IRX_DIR}/usbd.irx ${IRX_GEN_DIR}/usbd_irx.c usbd_irx
            DEPENDS ${IRX_DIR}/usbd.irx
            COMMENT "Embedding usbd.irx"
        )
        add_custom_command(
            OUTPUT ${IRX_GEN_DIR}/ps2kbd_irx.c
            COMMAND $ENV{PS2SDK}/bin/bin2c ${IRX_DIR}/ps2kbd.irx ${IRX_GEN_DIR}/ps2kbd_irx.c ps2kbd_irx
            DEPENDS ${IRX_DIR}/ps2kbd.irx
            COMMENT "Embedding ps2kbd.irx"
        )
        if(ENABLE_PS2_AUDIO)
            add_custom_command(
                OUTPUT ${IRX_GEN_DIR}/freesd_irx.c
                COMMAND $ENV{PS2SDK}/bin/bin2c ${IRX_DIR}/freesd.irx ${IRX_GEN_DIR}/freesd_irx.c freesd_irx
                DEPENDS ${IRX_DIR}/freesd.irx
                COMMENT "Embedding freesd.irx"
            )
            add_custom_command(
                OUTPUT ${IRX_GEN_DIR}/audsrv_irx.c
                COMMAND $ENV{PS2SDK}/bin/bin2c ${IRX_DIR}/audsrv.irx ${IRX_GEN_DIR}/audsrv_irx.c audsrv_irx
                DEPENDS ${IRX_DIR}/audsrv.irx
                COMMENT "Embedding audsrv.irx"
            )
        endif()

        target_sources(butterscotch PRIVATE ${IRX_GEN_DIR}/freesio2_irx.c ${IRX_GEN_DIR}/mcman_irx.c ${IRX_GEN_DIR}/mcserv_irx.c ${IRX_GEN_DIR}/freepad_irx.c ${IRX_GEN_DIR}/usbd_irx.c ${IRX_GEN_DIR}/ps2kbd_irx.c)
        if(ENABLE_PS2_AUDIO)
            target_sources(butterscotch PRIVATE ${IRX_GEN_DIR}/freesd_irx.c ${IRX_GEN_DIR}/audsrv_irx.c)
        endif()
        target_include_directories(butterscotch PRIVATE ${IRX_GEN_DIR})

        # ===[ gprof Profiling Support ]===
        option(ENABLE_GPROF_PROFILING "Enable gprof profiling for PS2" OFF)
        if(ENABLE_GPROF_PROFILING)
            add_compile_definitions(GPROF_PROFILING)
            target_compile_options(butterscotch PRIVATE -pg)
            target_link_options(butterscotch PRIVATE -pg)

            add_custom_command(
                OUTPUT ${IRX_GEN_DIR}/bdm_irx.c
                COMMAND $ENV{PS2SDK}/bin/bin2c ${IRX_DIR}/bdm.irx ${IRX_GEN_DIR}/bdm_irx.c bdm_irx
                DEPENDS ${IRX_DIR}/bdm.irx
                COMMENT "Embedding bdm.irx (profiler)"
            )
            add_custom_command(
                OUTPUT ${IRX_GEN_DIR}/bdmfs_fatfs_irx.c
                COMMAND $ENV{PS2SDK}/bin/bin2c ${IRX_DIR}/bdmfs_fatfs.irx ${IRX_GEN_DIR}/bdmfs_fatfs_irx.c bdmfs_fatfs_irx
                DEPENDS ${IRX_DIR}/bdmfs_fatfs.irx
                COMMENT "Embedding bdmfs_fatfs.irx (profiler)"
            )
            add_custom_command(
                OUTPUT ${IRX_GEN_DIR}/usbmass_bd_irx.c
                COMMAND $ENV{PS2SDK}/bin/bin2c ${IRX_DIR}/usbmass_bd.irx ${IRX_GEN_DIR}/usbmass_bd_irx.c usbmass_bd_irx
                DEPENDS ${IRX_DIR}/usbmass_bd.irx
                COMMENT "Embedding usbmass_bd.irx (profiler)"
            )
            target_sources(butterscotch PRIVATE
                ${IRX_GEN_DIR}/bdm_irx.c
                ${IRX_GEN_DIR}/bdmfs_fatfs_irx.c
                ${IRX_GEN_DIR}/usbmass_bd_irx.c
            )
        endif()
    else()
        # This block is for when we are developing Butterscotch with CLion!
        # ps2sdk and gsKit scatter headers across per-module subdirectories;
        # the flat include/ layout only exists after building the SDK.
        # We use file(GLOB) to discover all include dirs automatically.
        file(GLOB PS2SDK_EE_INCLUDES ${CMAKE_SOURCE_DIR}/ps2sdk/ee/*/include)
        file(GLOB PS2SDK_EE_RPC_INCLUDES ${CMAKE_SOURCE_DIR}/ps2sdk/ee/rpc/*/include)
        file(GLOB PS2SDK_EE_NETWORK_INCLUDES ${CMAKE_SOURCE_DIR}/ps2sdk/ee/network/*/include)
        file(GLOB GSKIT_INCLUDES ${CMAKE_SOURCE_DIR}/gsKit/ee/*/include)

        # _EE is normally set by the PS2 toolchain; without it, tamtypes.h
        # refuses to define u64/u32/etc. We set it so CLion can resolve them.
        target_compile_definitions(butterscotch PRIVATE _EE)

        target_include_directories(butterscotch PRIVATE
                ${PS2SDK_EE_INCLUDES}
                ${PS2SDK_EE_RPC_INCLUDES}
                ${PS2SDK_EE_NETWORK_INCLUDES}
                ${CMAKE_SOURCE_DIR}/ps2sdk/common/include
                ${GSKIT_INCLUDES}
                # Newlib and GCC built-in headers are build artifacts (not in any repo),
                # so these are still extracted from Docker by setup.sh.
                ${CMAKE_SOURCE_DIR}/ps2dev-headers/newlib-include
                ${CMAKE_SOURCE_DIR}/ps2dev-headers/gcc-include
        )
    endif()
elseif(PLATFORM STREQUAL "n3ds")
    option(N3DS_ENABLE_LTO "Enable link-time optimization for the 3DS target" ON)
    option(N3DS_ENABLE_FAST_MATH "Enable fast-math on the 3DS target" OFF)
    option(N3DS_USE_FLOAT_REALS "Use float instead of double for GMLReal on the 3DS target" OFF)
    option(N3DS_DISABLE_RVALUE_INT64 "Store oversized int64 values as reals/int32 on the 3DS target" OFF)
    option(N3DS_ENABLE_PERF_LOGS "Enable periodic 3DS renderer perf logs" OFF)
    option(N3DS_DISABLE_BOTTOM_SCREEN "Disable 3DS bottom-screen rendering" OFF)

    if(DEFINED ENV{DEVKITPRO} AND NOT "$ENV{DEVKITPRO}" STREQUAL "")
        file(TO_CMAKE_PATH "$ENV{DEVKITPRO}" DEVKITPRO_ROOT)
    elseif(EXISTS "C:/devkitPro")
        file(TO_CMAKE_PATH "C:/devkitPro" DEVKITPRO_ROOT)
    else()
        message(FATAL_ERROR "DEVKITPRO is not set and C:/devkitPro was not found.")
    endif()

    target_include_directories(butterscotch PRIVATE
        ${DEVKITPRO_ROOT}/libctru/include
        ${CMAKE_SOURCE_DIR}/vendor
        ${CMAKE_SOURCE_DIR}/vendor/stb/ds
    )

    target_link_directories(butterscotch PRIVATE
        ${DEVKITPRO_ROOT}/libctru/lib
    )

    target_compile_options(butterscotch PRIVATE
        -O3
        -fomit-frame-pointer
        -ffunction-sections
        -fdata-sections
        $<$<CONFIG:MinSizeRel>:-Os>
    )
    target_link_options(butterscotch PRIVATE -Wl,--gc-sections)

    if(N3DS_ENABLE_LTO)
        target_compile_options(butterscotch PRIVATE -flto)
        target_link_options(butterscotch PRIVATE -flto)
    endif()
    if(N3DS_ENABLE_FAST_MATH)
        target_compile_options(butterscotch PRIVATE -ffast-math)
    endif()
    if(N3DS_USE_FLOAT_REALS)
        target_compile_definitions(butterscotch PRIVATE USE_FLOAT_REALS)
    endif()
    if(N3DS_DISABLE_RVALUE_INT64)
        target_compile_definitions(butterscotch PRIVATE NO_RVALUE_INT64)
    endif()
    if(N3DS_ENABLE_PERF_LOGS)
        target_compile_definitions(butterscotch PRIVATE N3DS_ENABLE_PERF_LOGS)
    endif()
    if(N3DS_DISABLE_BOTTOM_SCREEN)
        target_compile_definitions(butterscotch PRIVATE N3DS_DISABLE_BOTTOM_SCREEN)
    endif()

    target_link_libraries(butterscotch PRIVATE citro2d citro3d ctru m)
    set_target_properties(butterscotch PROPERTIES OUTPUT_NAME papyrus)

    set(N3DS_ROMFS_DIR "${CMAKE_SOURCE_DIR}/resources/3ds/romfs")
    set(N3DS_SMDH_FILE "${CMAKE_CURRENT_BINARY_DIR}/cinnamon.default.smdh")
    ctr_generate_smdh(
        OUTPUT "${N3DS_SMDH_FILE}"
        NAME "${BUTTERSCOTCH_3DS_NAME}"
        DESCRIPTION "${BUTTERSCOTCH_3DS_DESCRIPTION}"
        AUTHOR "${BUTTERSCOTCH_3DS_AUTHOR}"
        ICON "${CMAKE_SOURCE_DIR}/resources/3ds/meta/icon.png"
    )
    if(EXISTS "${N3DS_ROMFS_DIR}")
        ctr_create_3dsx(papyrus TARGET butterscotch SMDH "${N3DS_SMDH_FILE}" ROMFS "${N3DS_ROMFS_DIR}")
    else()
        ctr_create_3dsx(papyrus TARGET butterscotch SMDH "${N3DS_SMDH_FILE}")
    endif()
else()
    message(FATAL_ERROR "Unknown platform! ${PLATFORM}")
endif()
