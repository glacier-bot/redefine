cmake_minimum_required(VERSION 3.30)
project(redefine VERSION 0.3.0 LANGUAGES C CXX)

include(cmake/dependencies.cmake)

add_executable(redefine WIN32
    src/main.cpp
    src/application.cpp
    src/explorer_tracker.cpp
    src/markdown_preview.cpp
    src/notes_repository.cpp
    src/object_identity.cpp
    src/panel_window.cpp
    src/redefine.manifest
    src/redefine.rc
)

target_compile_definitions(redefine PRIVATE
    UNICODE
    _UNICODE
    WIN32_LEAN_AND_MEAN
    NOMINMAX
    _WIN32_WINNT=0x0A00
    NTDDI_VERSION=0x0A000000
)
target_compile_options(redefine PRIVATE /W4 /permissive- /Zc:__cplusplus)
target_link_libraries(redefine PRIVATE
    WIL::WIL
    md4c::md4c
    unofficial::sqlite3::sqlite3
    d2d1
    dwrite
    dwmapi
    ole32
    oleaut32
    shell32
    shlwapi
)
