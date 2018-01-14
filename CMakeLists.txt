cmake_minimum_required (VERSION 3.9)
project (mongofs C)

set (CMAKE_C_STANDARD 11)

add_executable (mongofs main.c)

add_definitions(-D_FILE_OFFSET_BITS=64)
add_definitions(-D_DARWIN_USE_64_BIT_INODE=1)

find_package (libmongoc-1.0 1.7 REQUIRED)

message ("--   mongoc found version \"${MONGOC_VERSION}\"")
message ("--   mongoc include path \"${MONGOC_INCLUDE_DIRS}\"")
message ("--   mongoc libraries \"${MONGOC_LIBRARIES}\"")

target_include_directories (mongofs PRIVATE "${MONGOC_INCLUDE_DIRS}")
target_link_libraries (mongofs PRIVATE "${MONGOC_LIBRARIES}")
target_compile_definitions (mongofs PRIVATE "${MONGOC_DEFINITIONS}")

# TODO: do something better
target_include_directories (mongofs PRIVATE "/usr/local/include/osxfuse/fuse")
target_link_libraries (mongofs PRIVATE "osxfuse")

add_executable(test-mongofs unittest.c)

add_definitions(-D_FILE_OFFSET_BITS=64)
add_definitions(-D_DARWIN_USE_64_BIT_INODE=1)

target_include_directories (test-mongofs PRIVATE "${MONGOC_INCLUDE_DIRS}")
target_link_libraries (test-mongofs PRIVATE "${MONGOC_LIBRARIES}")
target_compile_definitions (test-mongofs PRIVATE "${MONGOC_DEFINITIONS}")

# TODO: do something better
target_include_directories (test-mongofs PRIVATE "/usr/local/include/osxfuse/fuse")
target_link_libraries (test-mongofs PRIVATE "osxfuse")