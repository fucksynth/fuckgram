# This file is part of fuckgram, a Telegram Desktop client based on AyuGram.
#
# For license and copyright information please follow this link:
# https://github.com/fucksynth/fuckgram/blob/main/LICENSE

# Lua 5.4 for the plugin runtime, compiled as C++ so that Lua errors
# unwind with exceptions (LUAI_THROW) and C++ destructors still run.
# Because of that the Lua API has C++ linkage: include "lua.h" etc.
# directly, never through an extern "C" block or lua.hpp.

add_library(lib_lua STATIC)
init_target(lib_lua)

add_library(fuckgram::lib_lua ALIAS lib_lua)

set(lua_loc ${third_party_loc}/lua)

set(lua_sources
    lapi.c
    lauxlib.c
    lbaselib.c
    lcode.c
    lcorolib.c
    lctype.c
    ldblib.c
    ldebug.c
    ldo.c
    ldump.c
    lfunc.c
    lgc.c
    linit.c
    liolib.c
    llex.c
    lmathlib.c
    lmem.c
    loadlib.c
    lobject.c
    lopcodes.c
    loslib.c
    lparser.c
    lstate.c
    lstring.c
    lstrlib.c
    ltable.c
    ltablib.c
    ltm.c
    lundump.c
    lutf8lib.c
    lvm.c
    lzio.c
)

nice_target_sources(lib_lua ${lua_loc}
PRIVATE
    ${lua_sources}
)

list(TRANSFORM lua_sources PREPEND ${lua_loc}/ OUTPUT_VARIABLE lua_sources_full)
set_source_files_properties(${lua_sources_full} PROPERTIES LANGUAGE CXX)

target_include_directories(lib_lua
PUBLIC
    ${lua_loc}
)

if (MSVC)
    # Lua is warning-clean as C, not at MSVC's C++ warning level.
    target_compile_options(lib_lua
    PRIVATE
        /W1
        /wd4244 # conversion, possible loss of data
        /wd4267 # size_t to smaller type
        /wd4334 # shift result truncated
        /wd4702 # unreachable code
    )
endif()
