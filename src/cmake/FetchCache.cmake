# Caches FetchContent's downloaded *source trees* at the repo root so they
# survive a `rm -rf build/`. Done by setting FETCHCONTENT_SOURCE_DIR_<NAME>
# to point at the cached copies — that skips FetchContent's populate step
# entirely (no download, no subbuild), which is also what lets us share the
# cache across generators (Ninja for the main project, VS for SteamAPIProxy)
# without hitting the "generator mismatch in *-subbuild/" error.
#
# Layout:
#   <repo>/.deps/<name>-src/   <-- canonical source for each dependency
#
# First configure (cache empty): FETCHCONTENT_BASE_DIR is pinned so the
# initial populate lands at the shared location.
# Subsequent configures: FETCHCONTENT_SOURCE_DIR_<NAME> overrides take
# precedence — populate is bypassed, subbuild + build live in the per-tree
# default ${CMAKE_BINARY_DIR}/_deps and don't conflict across generators.
#
# Override the cache location at configure time:
#   cmake -S src -B build -DOST_DEPS_DIR=/path/to/shared/cache
#
# Idempotent.
if(_OST_FETCH_CACHE_INITIALISED)
    return()
endif()
set(_OST_FETCH_CACHE_INITIALISED TRUE)

if(NOT DEFINED OST_DEPS_DIR)
    # CMAKE_CURRENT_LIST_DIR is src/cmake; ../../.deps is the repo root.
    get_filename_component(OST_DEPS_DIR
        "${CMAKE_CURRENT_LIST_DIR}/../../.deps" ABSOLUTE)
endif()

# Reuse a cached source tree only when its expected project files are present.
# Interrupted FetchContent operations can leave empty <name>-src directories;
# treating those as valid caches suppresses population and later causes errors
# such as a missing protoc target or an empty Lua source list.
set(_OST_ALL_CACHED TRUE)
set(_OST_HAS_INCOMPLETE_CACHE FALSE)
foreach(_dep IN ITEMS lua detours spdlog protobuf tomlplusplus)
    string(TOUPPER "${_dep}" _UPPER)
    set(_src "${OST_DEPS_DIR}/${_dep}-src")
    if(_dep STREQUAL "lua")
        set(_marker "src/lua.h")
    elseif(_dep STREQUAL "detours")
        set(_marker "src/detours.h")
    elseif(_dep STREQUAL "protobuf")
        set(_marker "cmake/CMakeLists.txt")
    else()
        set(_marker "CMakeLists.txt")
    endif()

    if(IS_DIRECTORY "${_src}" AND EXISTS "${_src}/${_marker}")
        set(FETCHCONTENT_SOURCE_DIR_${_UPPER} "${_src}" CACHE PATH
            "Pre-populated ${_dep} source dir" FORCE)
    else()
        set(_OST_ALL_CACHED FALSE)
        if(IS_DIRECTORY "${_src}")
            set(_OST_HAS_INCOMPLETE_CACHE TRUE)
            # Clear a stale override pointing at this incomplete shared source.
            if(DEFINED FETCHCONTENT_SOURCE_DIR_${_UPPER})
                get_filename_component(_configured_src "${FETCHCONTENT_SOURCE_DIR_${_UPPER}}" ABSOLUTE)
                get_filename_component(_expected_src "${_src}" ABSOLUTE)
                if(_configured_src STREQUAL _expected_src)
                    unset(FETCHCONTENT_SOURCE_DIR_${_UPPER} CACHE)
                endif()
            endif()
            message(STATUS "FetchContent: ignoring incomplete ${_dep} cache at ${_src}")
        endif()
    endif()
endforeach()

if(_OST_ALL_CACHED)
    message(STATUS "FetchContent: reusing cached sources at ${OST_DEPS_DIR}")
elseif(_OST_HAS_INCOMPLETE_CACHE)
    # Avoid cloning into a leftover partial source directory. Use this build
    # tree's FetchContent cache for a clean retry instead.
    set(FETCHCONTENT_BASE_DIR "${CMAKE_BINARY_DIR}/_deps" CACHE PATH
        "FetchContent cache for recovering incomplete shared sources" FORCE)
    message(STATUS "FetchContent: recovering incomplete sources in ${FETCHCONTENT_BASE_DIR}")
else()
    # At least one dep still needs to be downloaded. Direct the populate
    # output to the shared cache so the next configure can reuse it.
    set(FETCHCONTENT_BASE_DIR "${OST_DEPS_DIR}" CACHE PATH
        "Shared FetchContent cache (sources + first-time builds)" FORCE)
    message(STATUS "FetchContent: populating missing sources into ${OST_DEPS_DIR}")
endif()

# Skip the periodic git-fetch / tarball revalidation on configures after
# the initial populate.
set(FETCHCONTENT_UPDATES_DISCONNECTED ON CACHE BOOL
    "Skip FetchContent update step on subsequent configures" FORCE)
