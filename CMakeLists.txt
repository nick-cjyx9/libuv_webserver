cmake_minimum_required(VERSION 3.24)

project(uv_webserver
    VERSION 3.1.0
    LANGUAGES C
)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# SET(CMAKE_BUILD_TYPE "Debug")
# SET(CMAKE_C_FLAGS_DEBUG "$ENV{CFLAGS} -Wall -Wextra -ggdb")
SET(CMAKE_C_FLAGS "$ENV{CFLAGS} -Wall -Wextra")

add_library(picohttpparser STATIC
    third_party/picohttpparser/picohttpparser.c
)

target_include_directories(picohttpparser PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party/picohttpparser
)

add_subdirectory(third_party/libuv EXCLUDE_FROM_ALL)
add_subdirectory(third_party/argparse EXCLUDE_FROM_ALL)

add_executable(uvecho src/uvecho.c)
target_link_libraries(uvecho PRIVATE uv_a argparse_static)

add_executable(uvserver
    src/uvserver.c
    src/connection.c
    src/http.c
    src/fs.c
)
target_include_directories(uvserver PRIVATE src)
target_link_libraries(uvserver PRIVATE uv_a argparse_static picohttpparser)
