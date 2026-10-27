cmake_minimum_required(VERSION 3.16)
project(mmpr_movie_gg2pc C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

find_package(SDL2 REQUIRED CONFIG)

file(GLOB GENERATED_SRC ${CMAKE_SOURCE_DIR}/generated/*.c)
if (NOT GENERATED_SRC)
    message(FATAL_ERROR "generated/ is empty. Run:  python tools/build_sources.py <path to your MMPR The Movie .gg ROM>")
endif()

add_executable(mmpr_movie_gg
    runtime/src/main.c
    runtime/src/memory.c
    runtime/src/io.c
    runtime/src/vdp.c
    runtime/src/psg.c
    runtime/src/dispatch.c
    runtime/src/interp.c
    runtime/src/machine.c
    runtime/src/state.c
    ${GENERATED_SRC}
)
target_include_directories(mmpr_movie_gg PRIVATE runtime/include generated)
target_link_libraries(mmpr_movie_gg PRIVATE SDL2::SDL2main SDL2::SDL2)

if (WIN32)
    enable_language(RC)
    target_sources(mmpr_movie_gg PRIVATE res/mmpr.rc)
    set_target_properties(mmpr_movie_gg PROPERTIES WIN32_EXECUTABLE ON)
    if (MINGW)
        target_link_options(mmpr_movie_gg PRIVATE -static-libgcc)
    endif()
    # copy SDL2.dll next to the exe
    add_custom_command(TARGET mmpr_movie_gg POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_FILE:SDL2::SDL2> $<TARGET_FILE_DIR:mmpr_movie_gg>)
endif()

if (CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(mmpr_movie_gg PRIVATE -O2 -Wall -Wextra)
endif()

if (UNIX)
    target_link_libraries(mmpr_movie_gg PRIVATE m)
endif()
