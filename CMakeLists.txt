cmake_minimum_required(VERSION 3.16)
project(paratrooper C)

# SDL2: install with vcpkg (vcpkg install sdl2) or point SDL2_DIR at the
# SDL2 development package (SDL2-devel-2.x-VC.zip contains cmake config files).
find_package(SDL2 REQUIRED CONFIG)

set(COMMON src/bios.c src/font8x8.c src/image.c)

add_executable(paratrooper WIN32 src/game.c src/main_sdl.c ${COMMON})
target_link_libraries(paratrooper PRIVATE SDL2::SDL2main SDL2::SDL2)

# Headless verification build (no SDL needed)
add_executable(paratrooper_test src/game.c src/main_test.c ${COMMON})

if(MSVC)
    target_compile_options(paratrooper PRIVATE /W3)
else()
    target_compile_options(paratrooper PRIVATE -Wall -Wextra -Wno-unused-label)
endif()
