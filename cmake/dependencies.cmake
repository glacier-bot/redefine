include(FetchContent)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
if(MSVC)
    add_compile_options(/utf-8)
endif()
set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
set(BUILD_TESTING OFF CACHE BOOL "" FORCE)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>" CACHE STRING "" FORCE)

set(ABSL_BUILD_TESTING OFF CACHE BOOL "" FORCE)
set(ABSL_ENABLE_INSTALL ON CACHE BOOL "" FORCE)
set(ABSL_BUILD_TEST_HELPERS OFF CACHE BOOL "" FORCE)
set(ABSL_MSVC_STATIC_RUNTIME ON CACHE BOOL "" FORCE)
set(ABSL_PROPAGATE_CXX_STD ON CACHE BOOL "" FORCE)
set(RE2_TEST OFF CACHE BOOL "" FORCE)
set(RE2_BENCHMARK OFF CACHE BOOL "" FORCE)
set(RE2_BUILD_TESTING OFF CACHE BOOL "" FORCE)
set(BUILD_MD2HTML_EXECUTABLE OFF CACHE BOOL "" FORCE)
set(WIL_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(WIL_BUILD_PACKAGING OFF CACHE BOOL "" FORCE)

FetchContent_Declare(abseil
    GIT_REPOSITORY https://github.com/abseil/abseil-cpp.git
    GIT_TAG 20260107.1
    GIT_SHALLOW TRUE
    GIT_PROGRESS TRUE
)
FetchContent_MakeAvailable(abseil)

FetchContent_Declare(re2
    GIT_REPOSITORY https://github.com/google/re2.git
    GIT_TAG 927f5d53caf8111721e734cf24724686bb745f55
    GIT_PROGRESS TRUE
)
FetchContent_MakeAvailable(re2)

FetchContent_Declare(md4c
    GIT_REPOSITORY https://github.com/mity/md4c.git
    GIT_TAG v0.6.0
    GIT_SHALLOW TRUE
    GIT_PROGRESS TRUE
)
FetchContent_MakeAvailable(md4c)
if(TARGET md4c AND NOT TARGET md4c::md4c)
    add_library(md4c::md4c ALIAS md4c)
endif()

FetchContent_Declare(wil
    GIT_REPOSITORY https://github.com/microsoft/wil.git
    GIT_TAG v1.0.260126.7
    GIT_SHALLOW TRUE
    GIT_PROGRESS TRUE
)
FetchContent_MakeAvailable(wil)

FetchContent_Declare(sqlite3
    URL https://sqlite.org/2026/sqlite-autoconf-3540100.tar.gz
    URL_HASH SHA512=3df4348f54f5cd0554eec6d91e1d08a70535b7b78f497b2c6f24910b5087f6ef70483c9e35fa388bd8f38483cb8d1cb84e98ccdd58b9f525a10e0cf48c8c6742
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
)
FetchContent_MakeAvailable(sqlite3)
file(COPY "${sqlite3_SOURCE_DIR}/sqlite3.h" "${sqlite3_SOURCE_DIR}/sqlite3ext.h" DESTINATION "${sqlite3_BINARY_DIR}/include")
add_library(sqlite3 STATIC "${sqlite3_SOURCE_DIR}/sqlite3.c")
target_include_directories(sqlite3 PUBLIC "${sqlite3_BINARY_DIR}/include")
add_library(unofficial::sqlite3::sqlite3 ALIAS sqlite3)
