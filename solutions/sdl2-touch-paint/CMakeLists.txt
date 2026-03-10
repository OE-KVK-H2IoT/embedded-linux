cmake_minimum_required(VERSION 3.16)
project(sdl2_touch_paint C)

set(CMAKE_C_STANDARD 11)

find_package(SDL2 REQUIRED)

add_executable(sdl2_touch_paint main.c)
target_include_directories(sdl2_touch_paint PRIVATE ${SDL2_INCLUDE_DIRS})
target_link_libraries(sdl2_touch_paint PRIVATE ${SDL2_LIBRARIES} m)
