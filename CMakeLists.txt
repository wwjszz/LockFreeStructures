cmake_minimum_required(VERSION 3.14)
project(LockFreeStructures CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED OFF)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${PROJECT_BINARY_DIR}/bin)

set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Wall -Wextra")
set(CMAKE_CXX_FLAGS_RELEASE "-O3 -DNDEBUG")
#set(CMAKE_CXX_FLAGS_RELEASE "-g -O0")

include(FetchContent)

# Set the download directory to the project's source thirdparty/googletest
# (Alternatively, use ${CMAKE_BINARY_DIR}/thirdparty to keep thirdparty in the build tree)
set(FETCHCONTENT_BASE_DIR ${CMAKE_SOURCE_DIR}/thirdparty/googletest)

FetchContent_Declare(
        googletest
        GIT_REPOSITORY https://github.com/google/googletest.git
        GIT_TAG        v1.14.0  # fixed version
)

# This will download at configure time and add_subdirectory(...), creating gtest/gmock targets
FetchContent_MakeAvailable(googletest)

# ============================================================================
# Compiler Options
# ============================================================================

# Include CheckCXXCompilerFlag module to check compiler flag support
include(CheckCXXCompilerFlag)

# Check if compiler supports -Wno-interference-size option
if (CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    # Check if the warning option is supported
    check_cxx_compiler_flag("-Wno-interference-size" COMPILER_SUPPORTS_WNO_INTERFERENCE_SIZE)

    if (COMPILER_SUPPORTS_WNO_INTERFERENCE_SIZE)
        add_compile_options(-Wno-interference-size)
        message(STATUS "Compiler supports -Wno-interference-size option, added")
    else ()
        message(STATUS "Compiler does not support -Wno-interference-size option, skipped")
    endif ()
endif ()

# ============================================================================
# include directories
# ============================================================================

include_directories(${PROJECT_SOURCE_DIR}/)

# ============================================================================
# targets
# ============================================================================

add_executable(normaltest
        tests/test.cpp
        tests/simplethread.cpp
)

add_executable(unittest
        tests/unittests.cpp
        tests/simplethread.cpp
)

enable_testing()

add_executable(samples_linearsearchmaptest tests/samples_linearsearchmaptest.cpp)
target_link_libraries(samples_linearsearchmaptest PRIVATE gtest_main)

add_executable(samples_hashtabletest tests/samples_hashtabletest.cpp)
target_link_libraries(samples_hashtabletest PRIVATE gtest_main)

add_executable(hashtabletest tests/hashtabletest.cpp)
target_link_libraries(hashtabletest PRIVATE gtest_main)

add_executable(freelisttest tests/freelisttest.cpp)
target_link_libraries(freelisttest PRIVATE gtest_main)

add_executable(blockpooltest tests/blockpooltest.cpp)
target_link_libraries(blockpooltest PRIVATE gtest_main)

add_executable(fastqueuetest tests/fastqueuetest.cpp tests/Debug/ThrowOn.cpp)
target_link_libraries(fastqueuetest PRIVATE gtest_main)

add_executable(fastqueuetest_exception tests/fastqueuetest_exception.cpp tests/Debug/ThrowOn.cpp)
target_link_libraries(fastqueuetest_exception PRIVATE gtest_main)

add_test(NAME samples_linearsearchmaptest COMMAND samples_hashtabletest)
add_test(NAME samples_hashtabletest COMMAND samples_hashtabletest)
add_test(NAME hashtabletest COMMAND hashtabletest)
add_test(NAME freelisttest COMMAND freelisttest)
add_test(NAME blockpooltest COMMAND blockpooltest)
add_test(NAME fastqueuetest COMMAND fastqueuetest)
add_test(NAME fastqueuetest_exception COMMAND fastqueuetest_exception)