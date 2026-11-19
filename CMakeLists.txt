cmake_minimum_required(VERSION 3.20)
project(RobloxExt LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if (MSVC)
    add_compile_options(/W4 /permissive- /utf-8 /EHsc)
    add_compile_definitions(_CRT_SECURE_NO_WARNINGS NOMINMAX WIN32_LEAN_AND_MEAN)
else()
    add_compile_options(-Wall -Wextra)
endif()

add_executable(RobloxExt
    src/main.cpp
    src/memory.cpp
    src/instance.cpp
    src/game.cpp
    src/world.cpp
    src/render.cpp
    src/ui.cpp
    src/features.cpp
    src/overlay.cpp
)

target_link_libraries(RobloxExt PRIVATE
    d3d11 dxgi d2d1 dwrite dwmapi user32 gdi32 shell32 psapi
)

set_target_properties(RobloxExt PROPERTIES
    WIN32_EXECUTABLE TRUE
    VS_DEBUGGER_WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
)

# No external dependencies: Direct2D + DirectWrite ship with Windows.
