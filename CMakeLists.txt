cmake_minimum_required (VERSION 3.5...3.30)

option(TURBO_BUILD_APP "Build main editor application" ON)
option(TURBO_BUILD_TESTS "Build and run tests" OFF)
option(TURBO_BUILD_EXAMPLES "Build example apps" OFF)
option(TURBO_USE_SYSTEM_TVISION "Use system-wide Turbo Vision instead of the submodule" OFF)
option(TURBO_USE_STATIC_RTL "Link against the static version of the runtime library (MSVC only)" OFF)
option(TURBO_OPTIMIZE_BUILD "Use Precompiled Headers and Unity Build for the core library" ON)
set(TURBO_COMMIT_SHA $ENV{TURBO_COMMIT_SHA} CACHE STRING "Commit SHA to be displayed in the 'About' dialog")

if (${CMAKE_VERSION} VERSION_GREATER_EQUAL "3.15.0")
    cmake_policy(SET CMP0091 NEW)
    if (TURBO_USE_STATIC_RTL)
        set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
    endif()
endif()

project (turbo)

set(TURBO_BINARY_DIR ${CMAKE_CURRENT_BINARY_DIR})

# Optional: optimize the standalone executable for minimum on-disk size.
#
# The app's own dependencies (tvision, scintilla, lexilla, libvterm, turbo-core)
# are already linked statically, so the binary is self-contained: on macOS it
# only references system frameworks/dylibs that ship with every install (a fully
# static binary is not possible there -- Apple provides no static libSystem).
# This option trades a little speed (-Oz) for a much smaller binary: it builds
# every translation unit for size, lets the linker dead-strip unreferenced
# code/data, and strips the symbol table from the final executable. Measured on
# arm64: ~4.3 MB (Release -O3) -> ~1.9 MB.
option(TURBO_MINIMIZE_SIZE "Optimize the turbo executable for minimum size" OFF)
if (TURBO_MINIMIZE_SIZE AND NOT MSVC)
    # -Oz is the clang spelling for "optimize aggressively for size"; GCC uses -Os.
    set(_turbo_size_opt "-Oz")
    if (CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
        set(_turbo_size_opt "-Os")
    endif()
    set(_turbo_size_flags
        "${_turbo_size_opt} -fvisibility=hidden -fvisibility-inlines-hidden -ffunction-sections -fdata-sections")
    # Applied globally (right after project()) so every contributing target --
    # including the tvision submodule and the object libraries -- is built for size.
    # The flags must also go on the per-config variables (RELEASE/MINSIZEREL/...)
    # because those land AFTER CMAKE_CXX_FLAGS on the command line: without this,
    # the active config's own -O3 would override our -Oz. Appending here makes the
    # size opt level the last one seen, so it wins regardless of CMAKE_BUILD_TYPE
    # (the empty entry covers builds with no build type set).
    foreach(_cfg "" _RELEASE _MINSIZEREL _RELWITHDEBINFO)
        string(APPEND CMAKE_C_FLAGS${_cfg} " ${_turbo_size_flags}")
        string(APPEND CMAKE_CXX_FLAGS${_cfg} " ${_turbo_size_flags}")
    endforeach()
    if (APPLE)
        string(APPEND CMAKE_EXE_LINKER_FLAGS " -Wl,-dead_strip")
    else()
        string(APPEND CMAKE_EXE_LINKER_FLAGS " -Wl,--gc-sections -Wl,--as-needed")
    endif()
endif()
# Note: not applied to MSVC. The MSVC artifact jobs build MinSizeRel (/O1) with a
# static runtime, and the linker already does /OPT:REF,ICF dead-stripping by
# default; whole-program optimization (/GL + /LTCG) was measured to give ~0 size
# change (x64 -0.5%, x86 +0.3%) for a large link-time cost, so it is not used.

include(GNUInstallDirs)

# Targets 'scintilla' (5.x core) and 'lexilla' (language lexers)
#
# Both are pulled from upstream as git submodules under deps/ (matching the
# tvision/json convention) so they can be bumped with `git submodule update`
# instead of re-vendoring. The TVision platform layer that backs Scintilla lives
# in source/turbo-core/platform/*.cc (it provides Surface/Window/Font/etc.), so
# only the platform-independent core (deps/scintilla/src) is compiled here.
#
# Scintilla core can use a Unity Build; the lexers cannot (each LexXxx.cxx relies
# on file-local names that collide when batched), so lexilla uses a PCH instead.

# Targets 'tree-sitter', 'ts-elixir', 'ts-heex'
#
# Lexilla ships no Elixir or HEEx lexer, and HEEx cannot sensibly be hand-lexed:
# it is a three-level embedding (Elixir -> ~H"""...""" -> HTML -> {...} ->
# Elixir). Tree-sitter parses rather than regex-matches, which is what makes the
# nesting tractable, and it is a plain C library with checked-in generated
# parsers -- no CLI, no node, no new language runtime.
option(TURBO_ENABLE_TREESITTER "Build the tree-sitter lexer (Elixir, HEEx)" ON)

if (TURBO_ENABLE_TREESITTER)
    set(TS_DIR        "${CMAKE_CURRENT_LIST_DIR}/deps/tree-sitter")
    set(TS_ELIXIR_DIR "${CMAKE_CURRENT_LIST_DIR}/deps/tree-sitter-elixir")
    set(TS_HEEX_DIR   "${CMAKE_CURRENT_LIST_DIR}/deps/tree-sitter-heex")
    set(TS_HTML_DIR   "${CMAKE_CURRENT_LIST_DIR}/deps/tree-sitter-html")
    set(TS_CSS_DIR    "${CMAKE_CURRENT_LIST_DIR}/deps/tree-sitter-css")
    set(TS_JS_DIR     "${CMAKE_CURRENT_LIST_DIR}/deps/tree-sitter-javascript")
    set(TS_PHP_DIR    "${CMAKE_CURRENT_LIST_DIR}/deps/tree-sitter-php")
    set(TS_BLADE_DIR  "${CMAKE_CURRENT_LIST_DIR}/deps/tree-sitter-blade")
    set(TS_TSX_DIR    "${CMAKE_CURRENT_LIST_DIR}/deps/tree-sitter-typescript")

    # Blade is the most expensive grammar by a distance -- its generated parser is
    # ~18 MB because half its parser states are flat tables (an acknowledged
    # upstream issue) -- and it drags php_only in with it. Together they roughly
    # double the binary, so unlike the rest they can be switched off.
    option(TURBO_TS_BLADE "Build the Blade (Laravel) lexer; adds ~3 MB to the binary" ON)

    # lib.c is an amalgamation: it #includes the other lib/src/*.c. Compiling it
    # *and* its siblings would duplicate every symbol, so it is the only input.
    add_library(tree-sitter OBJECT "${TS_DIR}/lib/src/lib.c")
    target_include_directories(tree-sitter
        PUBLIC  "${TS_DIR}/lib/include"
        PRIVATE "${TS_DIR}/lib/src"
    )
    set_target_properties(tree-sitter PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED ON)

    # One tree-sitter, one version -- the split below is NOT two copies of it.
    #
    # A grammar's src/tree_sitter/parser.h is not the runtime; it is the codegen
    # contract its generated parser.c was built against, and it ships inside the
    # grammar precisely so the grammar compiles standalone. The runtime
    # deliberately publishes only api.h (see lib/include/tree_sitter/), and it
    # accepts any grammar in an ABI window -- currently 13..15 -- so that
    # consumers need not regenerate every grammar in lockstep with it.
    #
    # Elixir's parser is generated at ABI 14, HEEx's at 15. Both talk to the one
    # runtime through the one api.h; they just each need their own src/ on the
    # include path while compiling, because a parser.c only compiles against the
    # header it was generated with. Two object libraries is simply the tidiest way
    # to give each its own include path. (No newer Elixir grammar exists at ABI 15
    # -- even upstream main is still 14 -- and forcing a match would mean
    # regenerating the parsers ourselves, which buys a toolchain dependency on the
    # tree-sitter CLI and a fork of 12.8 MB of generated code to avoid four lines
    # of CMake.)
    add_library(ts-elixir OBJECT
        "${TS_ELIXIR_DIR}/src/parser.c"
        "${TS_ELIXIR_DIR}/src/scanner.c"
    )
    target_include_directories(ts-elixir PRIVATE "${TS_ELIXIR_DIR}/src")

    add_library(ts-heex OBJECT "${TS_HEEX_DIR}/src/parser.c")
    target_include_directories(ts-heex PRIVATE "${TS_HEEX_DIR}/src")

    # html and css are injection targets rather than languages turbo lexes
    # directly: HEEx (and later Blade) embed them, and html in turn injects
    # javascript into <script> and css into <style>. Without them those regions
    # keep their host's styling, which is why <script> inside a .heex file used to
    # render as plain text. Together they are ~0.6 MB of generated C.
    add_library(ts-html OBJECT "${TS_HTML_DIR}/src/parser.c" "${TS_HTML_DIR}/src/scanner.c")
    target_include_directories(ts-html PRIVATE "${TS_HTML_DIR}/src")

    add_library(ts-css OBJECT "${TS_CSS_DIR}/src/parser.c" "${TS_CSS_DIR}/src/scanner.c")
    target_include_directories(ts-css PRIVATE "${TS_CSS_DIR}/src")

    add_library(ts-js OBJECT "${TS_JS_DIR}/src/parser.c" "${TS_JS_DIR}/src/scanner.c")
    target_include_directories(ts-js PRIVATE "${TS_JS_DIR}/src")

    # tree-sitter-typescript exposes two languages, 'typescript' and 'tsx', from
    # separate generated parsers of ~8.5 MB each. Only tsx is built, and it serves
    # .ts as well as .tsx: it is a superset of typescript bar the legacy
    # angle-bracket cast (<Foo>bar), which TypeScript itself forbids in .tsx files
    # and which `as Foo` replaced. That saves 8.5 MB of parser for one dead syntax.
    add_library(ts-tsx OBJECT "${TS_TSX_DIR}/tsx/src/parser.c" "${TS_TSX_DIR}/tsx/src/scanner.c")
    target_include_directories(ts-tsx PRIVATE "${TS_TSX_DIR}/tsx/src")

    # The other dialect of tree-sitter-php: 'php' is PHP interleaved with HTML (a
    # .php file), where everything outside <?php ?> is an opaque (text) node that
    # the grammar expects you to inject HTML into. 'php_only' -- built below with
    # Blade -- is for a file that is pure PHP.
    add_library(ts-php OBJECT "${TS_PHP_DIR}/php/src/parser.c" "${TS_PHP_DIR}/php/src/scanner.c")
    target_include_directories(ts-php PRIVATE "${TS_PHP_DIR}/php/src")

    if (TURBO_TS_BLADE)
        # tree-sitter-php ships two dialects off one grammar: 'php' (PHP with
        # interleaved HTML, for a .php file) and 'php_only' (the file is pure PHP).
        # Blade injects into php_only -- its {{ }} and @php blocks are PHP with no
        # HTML around them -- so that is the one built here. Their scanners are thin
        # shims over a shared common/scanner.h, whose helpers are all static, so
        # linking both dialects would not clash if 'php' is added later.
        add_library(ts-php-only OBJECT
            "${TS_PHP_DIR}/php_only/src/parser.c"
            "${TS_PHP_DIR}/php_only/src/scanner.c"
        )
        target_include_directories(ts-php-only PRIVATE "${TS_PHP_DIR}/php_only/src")

        add_library(ts-blade OBJECT
            "${TS_BLADE_DIR}/src/parser.c"
            "${TS_BLADE_DIR}/src/scanner.c"
        )
        target_include_directories(ts-blade PRIVATE "${TS_BLADE_DIR}/src")
    endif()

    if (MSVC)
        # Elixir's generated parser is a single 12.8 MB / 428k-line translation
        # unit (7001 parser states). Without /bigobj MSVC fails it with C1128,
        # "number of sections exceeded object file format limit". The Windows jobs
        # are the ones that produce the release artifacts, so this is load-bearing.
        target_compile_options(ts-elixir PRIVATE /bigobj)
        target_compile_options(ts-heex PRIVATE /bigobj)
        target_compile_options(ts-html PRIVATE /bigobj)
        target_compile_options(ts-css PRIVATE /bigobj)
        target_compile_options(ts-js PRIVATE /bigobj)
        target_compile_options(ts-tsx PRIVATE /bigobj)
        target_compile_options(ts-php PRIVATE /bigobj)
        if (TURBO_TS_BLADE)
            target_compile_options(ts-php-only PRIVATE /bigobj)
            target_compile_options(ts-blade PRIVATE /bigobj)
        endif()
    endif()

    # The grammars' generated parsers are huge (Elixir's is ~12.8 MB / 7000 parser
    # states) and force -O0 on themselves with a #pragma, so a slow compile of
    # these two files is expected and not worth fighting.

    # Embed the grammars' own .scm queries by generating a TU from them, rather
    # than pasting them into turbo's source: they stay authoritative in the
    # submodule and cannot go stale against the grammar.
    file(READ "${TS_ELIXIR_DIR}/queries/highlights.scm"    TS_Q_ELIXIR_HL)
    file(READ "${TS_ELIXIR_DIR}/queries/injections.scm"    TS_Q_ELIXIR_INJ)
    file(READ "${TS_HEEX_DIR}/queries/highlights.scm"      TS_Q_HEEX_HL)
    file(READ "${TS_HEEX_DIR}/queries/injections.scm"      TS_Q_HEEX_INJ)
    file(READ "${TS_HTML_DIR}/queries/highlights.scm"      TS_Q_HTML_HL)
    file(READ "${TS_HTML_DIR}/queries/injections.scm"      TS_Q_HTML_INJ)
    file(READ "${TS_CSS_DIR}/queries/highlights.scm"       TS_Q_CSS_HL)
    file(READ "${TS_JS_DIR}/queries/highlights.scm"        TS_Q_JS_HL)
    # JSX lives in its own file and must be concatenated onto the main one.
    file(READ "${TS_JS_DIR}/queries/highlights-jsx.scm"    TS_Q_JS_JSX)
    file(READ "${TS_JS_DIR}/queries/injections.scm"        TS_Q_JS_INJ)
    # TypeScript ships no injections and only a *delta* highlights file (types,
    # interface, declare...), so tsx's queries are JavaScript's plus this.
    file(READ "${TS_TSX_DIR}/queries/highlights.scm"       TS_Q_TS_HL)
    file(READ "${TS_PHP_DIR}/queries/highlights.scm"       TS_Q_PHP_HL)
    file(READ "${TS_PHP_DIR}/queries/injections.scm"       TS_Q_PHP_INJ)
    # The HTML injection for a .php file is NOT in injections.scm -- it lives in
    # its own file. Wire only injections.scm and the HTML half of every .php file
    # renders as plain text, which would be a regression on LexHTML.
    file(READ "${TS_PHP_DIR}/queries/injections-text.scm"  TS_Q_PHP_TEXT_INJ)
    if (TURBO_TS_BLADE)
        file(READ "${TS_BLADE_DIR}/queries/highlights.scm" TS_Q_BLADE_HL)
        file(READ "${TS_BLADE_DIR}/queries/injections.scm" TS_Q_BLADE_INJ)
    endif()
    set(TS_QUERIES_CXX "${CMAKE_CURRENT_BINARY_DIR}/generated/TSQueries.cxx")
    configure_file(
        "${CMAKE_CURRENT_LIST_DIR}/source/turbo-core/lexers/TSQueries.cxx.in"
        "${TS_QUERIES_CXX}"
        @ONLY
    )
endif()

file(GLOB SCINTILLA_SRC "${CMAKE_CURRENT_LIST_DIR}/deps/scintilla/src/*.cxx")

file(GLOB LEXILLA_SRC
    "${CMAKE_CURRENT_LIST_DIR}/deps/lexilla/lexlib/*.cxx"
    "${CMAKE_CURRENT_LIST_DIR}/deps/lexilla/lexers/*.cxx"
)
list(APPEND LEXILLA_SRC "${CMAKE_CURRENT_LIST_DIR}/deps/lexilla/src/Lexilla.cxx")

# turbo-owned lexers, for languages Lexilla has none for. They build with the
# lexilla target rather than turbo-core for two reasons: turbo-core is a unity
# build and lexers rely on file-local names that collide when batched (the same
# reason lexilla itself uses a PCH instead), and they need lexilla's lexlib
# headers. They are named .cxx so the turbo-core *.cc glob leaves them alone.
list(APPEND LEXILLA_SRC
    "${CMAKE_CURRENT_LIST_DIR}/source/turbo-core/lexers/TurboLexers.cxx"
)
if (TURBO_ENABLE_TREESITTER)
    list(APPEND LEXILLA_SRC
        "${CMAKE_CURRENT_LIST_DIR}/source/turbo-core/lexers/TSLexer.cxx"
        "${TS_QUERIES_CXX}"
    )
endif()

add_library(scintilla OBJECT ${SCINTILLA_SRC})
add_library(lexilla OBJECT ${LEXILLA_SRC})

function(turbo_set_warnings t)
    if (NOT ("${CMAKE_CXX_COMPILER_ID}" STREQUAL "MSVC"))
        target_compile_options(${t} PRIVATE
            -Wall
        )
    else()
        target_compile_options(${t} PRIVATE
            /wd4244 # Possible data loss in type conversion
        )
    endif()
endfunction()

foreach(t scintilla lexilla)
    target_compile_features(${t} PUBLIC cxx_std_17)
    turbo_set_warnings(${t})
    if ("${CMAKE_CXX_COMPILER_ID}" STREQUAL "MSVC")
        target_compile_options(${t} PRIVATE
            /wd4514 /wd4166 /wd4068 /wd4996 /wd4250 /wd4267
            /permissive- /Zc:__cplusplus
        )
    endif()
    # CURSES selects PLAT_CURSES in Scintilla's Platform.h (the closest match to a
    # terminal); the concrete platform implementation is turbo-core's own.
    target_compile_definitions(${t} PRIVATE CURSES)
endforeach()

target_include_directories(scintilla PRIVATE
    "${CMAKE_CURRENT_LIST_DIR}/deps/scintilla/include"
    "${CMAKE_CURRENT_LIST_DIR}/deps/scintilla/src"
)

target_include_directories(lexilla PRIVATE
    "${CMAKE_CURRENT_LIST_DIR}/deps/scintilla/include"
    "${CMAKE_CURRENT_LIST_DIR}/deps/lexilla/include"
    "${CMAKE_CURRENT_LIST_DIR}/deps/lexilla/lexlib"
    # turbo-owned lexers (source/turbo-core/lexers) share the style ids with
    # styles.cc through <turbo/lexts.h>.
    "${CMAKE_CURRENT_LIST_DIR}/include"
)
if (TURBO_ENABLE_TREESITTER)
    # Only tree-sitter's public api.h -- deliberately NOT either grammar's src/,
    # which would drag in a conflicting tree_sitter/parser.h (see above).
    target_include_directories(lexilla PRIVATE "${TS_DIR}/lib/include")
    target_compile_definitions(lexilla PRIVATE TURBO_ENABLE_TREESITTER)
    if (TURBO_TS_BLADE)
        target_compile_definitions(lexilla PRIVATE TURBO_TS_BLADE)
    endif()
endif()

# Target 'vterm' (libvterm: VT/ANSI terminal-emulation core, MIT-licensed)
#
# Pulled in as a git submodule under deps/ exactly like Scintilla/Lexilla, and
# compiled the same way: glob its C sources into an object library. libvterm is
# UI-less (it parses terminal output into a cell grid and encodes key input);
# the TVision terminal view in source/turbo feeds it PTY output and renders the
# grid. The generated encoding tables (src/encoding/*.inc, src/fullwidth.inc)
# are committed upstream, so there is no code-generation step to run.
file(GLOB VTERM_SRC "${CMAKE_CURRENT_LIST_DIR}/deps/libvterm/src/*.c")
add_library(vterm OBJECT ${VTERM_SRC})
set_target_properties(vterm PROPERTIES C_STANDARD 99 C_STANDARD_REQUIRED ON)
target_include_directories(vterm PUBLIC
    "$<BUILD_INTERFACE:${CMAKE_CURRENT_LIST_DIR}/deps/libvterm/include>"
)
target_include_directories(vterm PRIVATE
    "${CMAKE_CURRENT_LIST_DIR}/deps/libvterm/src"
)

# Target 'lua' (Lua 5.4 interpreter, MIT-licensed)
#
# Embedded so the editor can be scripted and configured in Lua. Pulled in as a
# git submodule under deps/ (pinned to a release tag) and compiled like vterm:
# glob its C sources into an object library, which is then folded into the
# 'turbo' app (the LuaManager that drives it lives in source/turbo). The Lua
# sources sit at the submodule root, not in a src/ subdir. We drop three .c
# files: lua.c (the standalone interpreter's main()), onelua.c (the single-TU
# amalgamation, which would duplicate every Lua symbol) and ltests.c (the
# internal test harness, which only compiles under -DLUA_USER_H="ltests.h").
file(GLOB LUA_SRC "${CMAKE_CURRENT_LIST_DIR}/deps/lua/*.c")
list(FILTER LUA_SRC EXCLUDE REGEX "(lua|ltests)\\.c$")
add_library(lua OBJECT ${LUA_SRC})
set_target_properties(lua PROPERTIES C_STANDARD 99 C_STANDARD_REQUIRED ON)
target_include_directories(lua PUBLIC
    "$<BUILD_INTERFACE:${CMAKE_CURRENT_LIST_DIR}/deps/lua>"
)
# Enable Lua's POSIX feature set (io.popen, os.* via mkstemp, isatty) on Unix.
# We deliberately do NOT define LUA_USE_DLOPEN: scripts are pure Lua, so we skip
# dynamic C-module loading and the -ldl link dependency it would pull in.
if (UNIX)
    target_compile_definitions(lua PRIVATE LUA_USE_POSIX)
endif()

# Dependencies

include(deps/CMakeLists.txt)

# Target 'turbo-core'

set(TURBO turbo)

file(GLOB_RECURSE TURBO_CORE_SRC "${CMAKE_CURRENT_LIST_DIR}/source/${TURBO}-core/*.cc")
set(TURBO_CORE_OBJECTS
    $<TARGET_OBJECTS:scintilla>
    $<TARGET_OBJECTS:lexilla>
)
if (TURBO_ENABLE_TREESITTER)
    list(APPEND TURBO_CORE_OBJECTS
        $<TARGET_OBJECTS:tree-sitter>
        $<TARGET_OBJECTS:ts-elixir>
        $<TARGET_OBJECTS:ts-heex>
        $<TARGET_OBJECTS:ts-html>
        $<TARGET_OBJECTS:ts-css>
        $<TARGET_OBJECTS:ts-js>
        $<TARGET_OBJECTS:ts-tsx>
        $<TARGET_OBJECTS:ts-php>
    )
    if (TURBO_TS_BLADE)
        list(APPEND TURBO_CORE_OBJECTS
            $<TARGET_OBJECTS:ts-php-only>
            $<TARGET_OBJECTS:ts-blade>
        )
    endif()
endif()
add_library(${TURBO}-core
    ${TURBO_CORE_SRC}
    ${TURBO_CORE_OBJECTS}
)

target_compile_features(${TURBO}-core PRIVATE cxx_std_17)
turbo_set_warnings(${TURBO}-core)
if (TURBO_ENABLE_TREESITTER)
    # styles.cc picks the tree-sitter lexer for JS/TS/PHP when it is built, and
    # the Lexilla one otherwise.
    target_compile_definitions(${TURBO}-core PRIVATE TURBO_ENABLE_TREESITTER)
endif()
target_include_directories(${TURBO}-core PUBLIC
    "$<BUILD_INTERFACE:${CMAKE_CURRENT_LIST_DIR}/include>"
    "$<INSTALL_INTERFACE:include>"
    # turbo's public header <turbo/scintilla.h> includes Scintilla's and Lexilla's
    # public C API headers (Scintilla.h, SciLexer.h), so consumers of turbo-core
    # (the app, examples, tests) must see these include dirs too.
    "$<BUILD_INTERFACE:${CMAKE_CURRENT_LIST_DIR}/deps/scintilla/include>"
    "$<BUILD_INTERFACE:${CMAKE_CURRENT_LIST_DIR}/deps/lexilla/include>"
)
target_include_directories(${TURBO}-core PRIVATE
    "${CMAKE_CURRENT_LIST_DIR}/source/${TURBO}-core"
    "${CMAKE_CURRENT_LIST_DIR}/source/${TURBO}-core/platform"
    # Scintilla's internal headers (src/) — needed only by turbo-core's platform
    # layer, which subclasses Scintilla internals.
    "${CMAKE_CURRENT_LIST_DIR}/deps/scintilla/src"
)
target_link_libraries(${TURBO}-core PUBLIC
    tvision
)

# The filesystem watcher uses FSEvents on macOS (CoreServices framework).
if (APPLE)
    target_link_libraries(${TURBO}-core PUBLIC "-framework CoreServices")
endif()

# The terminal's PTY backend uses forkpty(). On Linux/BSD it lives in libutil;
# on macOS it is part of libSystem (no extra link needed); on Windows it is a stub.
if (UNIX AND NOT APPLE)
    target_link_libraries(${TURBO}-core PUBLIC util)
endif()

# CoreServices/Cocoa headers clash with tvision's `Boolean` typedef (bool vs
# unsigned char) when they share a translation unit. The watcher would see
# tvision via both the unity batch and the precompiled header, so opt it out of
# BOTH and compile it on its own with only its own includes + CoreServices.
set_source_files_properties(
    "${CMAKE_CURRENT_LIST_DIR}/source/${TURBO}-core/filewatcher.cc"
    PROPERTIES
        SKIP_UNITY_BUILD_INCLUSION ON
        SKIP_PRECOMPILE_HEADERS ON)

# pty.cc is pure POSIX system code (forkpty/termios) and includes no tvision; it
# has no business in the tvision-laden unity batch / PCH, so build it on its own.
set_source_files_properties(
    "${CMAKE_CURRENT_LIST_DIR}/source/${TURBO}-core/pty.cc"
    PROPERTIES
        SKIP_UNITY_BUILD_INCLUSION ON
        SKIP_PRECOMPILE_HEADERS ON)

# The MCP transport/bridge (source/turbo-core/mcp/*.cc) are pure POSIX socket
# code with no tvision; keep them out of the unity batch / PCH like pty.cc.
set_source_files_properties(
    "${CMAKE_CURRENT_LIST_DIR}/source/${TURBO}-core/mcp/transport.cc"
    "${CMAKE_CURRENT_LIST_DIR}/source/${TURBO}-core/mcp/bridge.cc"
    PROPERTIES
        SKIP_UNITY_BUILD_INCLUSION ON
        SKIP_PRECOMPILE_HEADERS ON)

# The MCP transport uses std::thread regardless of the LSP feature, so link the
# threading library unconditionally (the LSP block below is a no-op duplicate
# when enabled).
find_package(Threads REQUIRED)
target_link_libraries(${TURBO}-core PUBLIC Threads::Threads)

if (${CMAKE_VERSION} VERSION_GREATER_EQUAL "3.13.0")
    install(TARGETS ${TURBO}-core
        EXPORT ${TURBO}-config
        ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
        COMPONENT library
    )
    install(EXPORT ${TURBO}-config
        DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/${TURBO}
        NAMESPACE ${TURBO}::
        FILE ${TURBO}-config.cmake
        COMPONENT library
    )
    install(DIRECTORY "${CMAKE_CURRENT_LIST_DIR}/include/turbo" DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
    install(FILES "${CMAKE_CURRENT_LIST_DIR}/doc/turboIDE.1" DESTINATION share/man/man1)
endif()

# Optional dependencies

find_library(MAGIC magic)
if (MAGIC)
    target_link_libraries(${TURBO}-core PRIVATE ${MAGIC})
    target_compile_definitions(${TURBO}-core PRIVATE HAVE_MAGIC)
endif()

# nlohmann/json is used by the project build-config storage (and the LSP
# client), so make its single header available to turbo-core and everything that
# links it -- regardless of whether LSP is enabled.
target_include_directories(${TURBO}-core PUBLIC
    "$<BUILD_INTERFACE:${CMAKE_CURRENT_LIST_DIR}/deps/json/single_include>"
)

# Optional: Language Server Protocol support.
option(TURBO_ENABLE_LSP "Enable Language Server Protocol support" ON)
if (TURBO_ENABLE_LSP)
    find_package(Threads REQUIRED)
    # PUBLIC so the 'turbo' app target inherits the feature macro (its LSP glue
    # includes the turbo-core LSP headers).
    target_link_libraries(${TURBO}-core PUBLIC Threads::Threads)
    target_compile_definitions(${TURBO}-core PUBLIC TURBO_ENABLE_LSP)
endif()

# Optional: Debug Adapter Protocol support (debugger integration). Independent
# of LSP -- its transport (source/turbo-core/dap) has its own Content-Length
# framing and reader/writer threads, so it does not require TURBO_ENABLE_LSP.
option(TURBO_ENABLE_DAP "Enable Debug Adapter Protocol support" ON)
if (TURBO_ENABLE_DAP)
    find_package(Threads REQUIRED)
    # PUBLIC so the 'turbo' app target inherits the feature macro (its DAP glue
    # includes the turbo-core DAP headers).
    target_link_libraries(${TURBO}-core PUBLIC Threads::Threads)
    target_compile_definitions(${TURBO}-core PUBLIC TURBO_ENABLE_DAP)
endif()

# Target 'turbo'

if (TURBO_BUILD_APP)
    file(GLOB_RECURSE TURBO_SRC "${CMAKE_CURRENT_LIST_DIR}/source/${TURBO}/*.cc")
    # The terminal window (source/turbo/terminal.cc) drives libvterm and the
    # LuaManager (source/turbo/luamanager.cc) drives the embedded Lua; fold both
    # sets of object files into the app and expose their public headers.
    add_executable(${TURBO} ${TURBO_SRC} $<TARGET_OBJECTS:vterm> $<TARGET_OBJECTS:lua>)
    # The internal target/source names stay 'turbo' (source/turbo, turbo-core,
    # install exports), but the shipped binary is 'turboIDE'. $<TARGET_FILE>,
    # install, strip, RC and the macOS icon step all resolve the real output
    # path, so they follow this rename automatically.
    set_target_properties(${TURBO} PROPERTIES OUTPUT_NAME turboIDE)
    # buildconfig.cc pulls in the large nlohmann/json header; keep it out of the
    # unity batch so it compiles once on its own instead of slowing a whole batch.
    set_source_files_properties(
        "${CMAKE_CURRENT_LIST_DIR}/source/${TURBO}/buildconfig.cc"
        PROPERTIES SKIP_UNITY_BUILD_INCLUSION ON)
    # luamanager.cc includes the Lua C headers (macro-heavy luaconf.h); keep it
    # out of the unity batch so those macros can't leak into batch-mates.
    set_source_files_properties(
        "${CMAKE_CURRENT_LIST_DIR}/source/${TURBO}/luamanager.cc"
        PROPERTIES SKIP_UNITY_BUILD_INCLUSION ON)
    # mcpserver.cc pulls in the large nlohmann/json header; keep it out of the
    # unity batch (like buildconfig.cc).
    set_source_files_properties(
        "${CMAKE_CURRENT_LIST_DIR}/source/${TURBO}/mcpserver.cc"
        PROPERTIES SKIP_UNITY_BUILD_INCLUSION ON)
    # dapmanager.cc pulls in the large nlohmann/json header (via the DAP client);
    # keep it out of the unity batch (like buildconfig.cc / mcpserver.cc).
    set_source_files_properties(
        "${CMAKE_CURRENT_LIST_DIR}/source/${TURBO}/dapmanager.cc"
        PROPERTIES SKIP_UNITY_BUILD_INCLUSION ON)
    # debugconfig.cc pulls in the large nlohmann/json header (like buildconfig.cc).
    set_source_files_properties(
        "${CMAKE_CURRENT_LIST_DIR}/source/${TURBO}/debugconfig.cc"
        PROPERTIES SKIP_UNITY_BUILD_INCLUSION ON)
    target_compile_features(${TURBO} PRIVATE cxx_std_17)
    turbo_set_warnings(${TURBO})
    target_link_libraries(${TURBO} PRIVATE
        ${TURBO}-core
    )
    # terminal.cc includes <vterm.h> and luamanager.cc includes <lua.h>; the
    # objects for both are folded in above.
    target_include_directories(${TURBO} PRIVATE
        "${CMAKE_CURRENT_LIST_DIR}/deps/libvterm/include"
        "${CMAKE_CURRENT_LIST_DIR}/deps/lua"
    )
    install(TARGETS ${TURBO} RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})

    # Strip the symbol table from the final binary when minimizing size. Attached
    # before the macOS icon POST_BUILD so it runs first (POST_BUILD commands run
    # in attachment order, and stripping rewrites the Mach-O).
    if (TURBO_MINIMIZE_SIZE AND CMAKE_STRIP)
        add_custom_command(TARGET ${TURBO} POST_BUILD
            COMMAND ${CMAKE_STRIP} $<TARGET_FILE:${TURBO}>
            COMMENT "Stripping symbols from ${TURBO} (TURBO_MINIMIZE_SIZE)")
    endif()

    if (NOT "${TURBO_COMMIT_SHA}" STREQUAL "")
        string(SUBSTRING "${TURBO_COMMIT_SHA}" 0 8 TURBO_COMMIT_SHORT_SHA)
        target_compile_definitions(${TURBO} PRIVATE TURBO_VERSION_STRING="${TURBO_COMMIT_SHORT_SHA}")
    endif()

    if (WIN32)
        file(GLOB_RECURSE TURBO_RC "${CMAKE_CURRENT_LIST_DIR}/source/${TURBO}/*.rc")
        target_sources(${TURBO} PRIVATE ${TURBO_RC})

        if (MSVC)
            # Wildcard argument expansion
            set_target_properties(${TURBO} PROPERTIES LINK_FLAGS wsetargv.obj)
        endif()
    elseif (APPLE)
        add_custom_command(TARGET ${TURBO} POST_BUILD COMMAND
            # Script from https://github.com/mklement0/fileicon
            osascript
                -e "use framework \\\"Cocoa\\\""
                -e "set sourcePath to \\\"${CMAKE_CURRENT_LIST_DIR}/source/${TURBO}/icon.icns\\\""
                -e "set destPath to \\\"$<TARGET_FILE:${TURBO}>\\\""
                -e "set imageData to \\(current application\\'s NSImage\\'s alloc\\(\\)\\'s initWithContentsOfFile:sourcePath\\)"
                -e "\\(current application\\'s NSWorkspace\\'s sharedWorkspace\\(\\)\\'s setIcon:imageData forFile:destPath options:0\\)"
            ">/dev/null" "||" echo "Warning: failed to set application icon"
        )
    endif()
endif()

# Target 'tests'

if (TURBO_BUILD_TESTS)
    file(GLOB_RECURSE TEST_SRC "${CMAKE_CURRENT_LIST_DIR}/test/*.cc")
    add_executable(${TURBO}-test ${TEST_SRC})
    target_compile_features(${TURBO}-test PRIVATE cxx_std_17)
    turbo_set_warnings(${TURBO}-test)
    # test/test.h includes <turbo/scintilla/internals.h>, which pulls in
    # Scintilla's internal headers (Debugging.h etc.). turbo-core keeps
    # deps/scintilla/src PRIVATE, so the test target must add it explicitly.
    target_include_directories(${TURBO}-test PRIVATE
        "${CMAKE_CURRENT_LIST_DIR}/deps/scintilla/src")
    find_library(GTEST gtest REQUIRED)
    find_library(GTEST_MAIN gtest_main REQUIRED)
    # The gtest headers are not on the default search path everywhere (e.g.
    # Homebrew's prefix on macOS); resolve them like the libraries above.
    find_path(GTEST_INCLUDE gtest/gtest.h REQUIRED)
    target_include_directories(${TURBO}-test PRIVATE ${GTEST_INCLUDE})
    target_link_libraries(${TURBO}-test PRIVATE
        ${TURBO}-core
        ${GTEST}
        ${GTEST_MAIN}
    )

    if (NOT WIN32)
        find_library(PTHREAD pthread REQUIRED)
        tv_message(STATUS "Found 'pthread': ${PTHREAD}")
        target_link_libraries(${TURBO}-test PRIVATE
            ${PTHREAD}
        )
    endif()

    add_custom_command(
        OUTPUT ${TURBO}-test-passed
        COMMAND ${TURBO}-test
        COMMAND ${CMAKE_COMMAND} -E touch ${TURBO}-test-passed
        DEPENDS ${TURBO}-test
        WORKING_DIRECTORY ${TURBO_BINARY_DIR}
    )
    add_custom_target(${TURBO}-test-run ALL DEPENDS ${TURBO}-test-passed)
    if (NOT TURBO_USE_SYSTEM_TVISION)
        add_dependencies(${TURBO}-test-run tvision-test-run)
    endif()
endif()

# Examples

add_subdirectory(source/examples)

# Optional build optimization

if(${CMAKE_VERSION} VERSION_GREATER_EQUAL "3.16.0" AND TURBO_OPTIMIZE_BUILD)
    set_target_properties(${TURBO}-core PROPERTIES UNITY_BUILD ON)
    target_precompile_headers(${TURBO}-core PRIVATE
        "${CMAKE_CURRENT_LIST_DIR}/include/TurboPCH.h"
    )
    set_target_properties(scintilla PROPERTIES UNITY_BUILD ON)
    target_precompile_headers(lexilla PRIVATE
        "${CMAKE_CURRENT_LIST_DIR}/include/ScilexersPCH.h"
    )
    if (TURBO_BUILD_APP)
        set_target_properties(${TURBO} PROPERTIES UNITY_BUILD ON)
    endif()
    if (TURBO_BUILD_TESTS)
        set_target_properties(${TURBO}-test PROPERTIES UNITY_BUILD ON)
    endif()
endif()
