# enkiTS 1.11 (https://github.com/dougbinks/enkiTS, commit 03e6a2c, zlib licence in License.txt):
# the task scheduler behind engine_core's TaskSystem. The C++ sources only, unmodified, built as a
# static library here rather than taken from vcpkg: the worktrees share one vcpkg_installed tree, and
# a configure from a checkout whose manifest lacks a port removes it for all of them.
add_library(enkiTS STATIC
    LockLessMultiReadPipe.h
    TaskScheduler.cpp
    TaskScheduler.h
)
add_library(enkiTS::enkiTS ALIAS enkiTS)

target_include_directories(enkiTS
    PUBLIC
        "${CMAKE_CURRENT_SOURCE_DIR}"
)
target_compile_definitions(enkiTS
    PUBLIC
        ENKITS_TASK_PRIORITIES_NUM=3
)
find_package(Threads REQUIRED)
target_link_libraries(enkiTS
    PUBLIC
        Threads::Threads
)

set_target_properties(enkiTS PROPERTIES FOLDER "ThirdParty")
