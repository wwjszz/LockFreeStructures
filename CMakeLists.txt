cmake_minimum_required(VERSION 3.20)

project(LockFreeStructures LANGUAGES CXX)

include(CTest)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

option(LOCKFREESTRUCTURES_BUILD_BENCHMARKS
       "Build queue comparison benchmarks"
       OFF)

# Header-only library target used by tests and downstream projects.
add_library(lockfree_structures INTERFACE)
add_library(LockFreeStructures::lockfree_structures ALIAS lockfree_structures)

target_include_directories(
  lockfree_structures
  INTERFACE
    "$<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}>"
)

target_compile_features(
  lockfree_structures
  INTERFACE
    cxx_std_20
)

function(lockfree_enable_warnings target_name)
  if(MSVC)
    target_compile_options(
      "${target_name}"
      PRIVATE
        /W4
        /permissive-
        /utf-8
    )
  elseif(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(
      "${target_name}"
      PRIVATE
        -Wall
        -Wextra
        -Wpedantic
    )
  endif()
endfunction()

find_package(Threads REQUIRED)

if(BUILD_TESTING)
  set(TEST_BIN_DIR "${PROJECT_BINARY_DIR}/test/bin")

  file(
    GLOB TEST_SOURCES
    CONFIGURE_DEPENDS
      "${PROJECT_SOURCE_DIR}/test/test_*.cc"
      "${PROJECT_SOURCE_DIR}/test/test_*.cpp"
      "${PROJECT_SOURCE_DIR}/test/test_*.cxx"
  )

  foreach(TEST_SOURCE IN LISTS TEST_SOURCES)
    get_filename_component(TEST_NAME "${TEST_SOURCE}" NAME_WE)

    add_executable("${TEST_NAME}" "${TEST_SOURCE}")

    target_link_libraries(
      "${TEST_NAME}"
      PRIVATE
        LockFreeStructures::lockfree_structures
        Threads::Threads
    )

    lockfree_enable_warnings("${TEST_NAME}")

    add_test(
      NAME "${TEST_NAME}"
      COMMAND "$<TARGET_FILE:${TEST_NAME}>"
    )

    set_tests_properties(
      "${TEST_NAME}"
      PROPERTIES
        TIMEOUT 30
        WORKING_DIRECTORY "${PROJECT_SOURCE_DIR}"
    )

    set_target_properties(
      "${TEST_NAME}"
      PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${TEST_BIN_DIR}"
        RUNTIME_OUTPUT_DIRECTORY_DEBUG "${TEST_BIN_DIR}"
        RUNTIME_OUTPUT_DIRECTORY_RELEASE "${TEST_BIN_DIR}"
        RUNTIME_OUTPUT_DIRECTORY_RELWITHDEBINFO "${TEST_BIN_DIR}"
        RUNTIME_OUTPUT_DIRECTORY_MINSIZEREL "${TEST_BIN_DIR}"
    )
  endforeach()
endif()

if(LOCKFREESTRUCTURES_BUILD_BENCHMARKS)
  include(FetchContent)

  # Keep dependency tests and optional components out of this project's build.
  set(BENCHMARK_ENABLE_TESTING OFF CACHE BOOL "" FORCE)
  set(BENCHMARK_ENABLE_INSTALL OFF CACHE BOOL "" FORCE)
  set(TBB_TEST OFF CACHE BOOL "" FORCE)
  set(TBB_EXAMPLES OFF CACHE BOOL "" FORCE)
  set(TBB_STRICT OFF CACHE BOOL "" FORCE)

  if(NOT TARGET benchmark::benchmark)
    find_package(benchmark CONFIG QUIET)
  endif()

  if(NOT TARGET benchmark::benchmark)
    if(EXISTS "${PROJECT_SOURCE_DIR}/thirdparty/benchmark/CMakeLists.txt")
      add_subdirectory(
        "${PROJECT_SOURCE_DIR}/thirdparty/benchmark"
        "${PROJECT_BINARY_DIR}/_deps/benchmark-build"
        EXCLUDE_FROM_ALL
      )
    else()
      FetchContent_Declare(
        googlebenchmark
        GIT_REPOSITORY https://github.com/google/benchmark.git
        GIT_TAG v1.9.0
        GIT_SHALLOW TRUE
      )
      FetchContent_MakeAvailable(googlebenchmark)
    endif()
  endif()

  if(NOT TARGET TBB::tbb)
    find_package(TBB CONFIG QUIET)
  endif()

  if(NOT TARGET TBB::tbb)
    if(EXISTS "${PROJECT_SOURCE_DIR}/thirdparty/onetbb/CMakeLists.txt")
      add_subdirectory(
        "${PROJECT_SOURCE_DIR}/thirdparty/onetbb"
        "${PROJECT_BINARY_DIR}/_deps/onetbb-build"
        EXCLUDE_FROM_ALL
      )
    else()
      FetchContent_Declare(
        onetbb
        GIT_REPOSITORY https://github.com/oneapi-src/oneTBB.git
        GIT_TAG v2021.13.0
        GIT_SHALLOW TRUE
      )
      FetchContent_MakeAvailable(onetbb)
    endif()
  endif()

  find_path(
    LOCKFREESTRUCTURES_BOOST_INCLUDE_DIR
    NAMES boost/lockfree/queue.hpp
    HINTS
      "${PROJECT_SOURCE_DIR}/thirdparty/boost_1_90_0"
      "$ENV{BOOST_ROOT}"
  )

  if(NOT LOCKFREESTRUCTURES_BOOST_INCLUDE_DIR)
    find_package(Boost 1.74 QUIET)
    if(Boost_FOUND)
      set(LOCKFREESTRUCTURES_BOOST_INCLUDE_DIR "${Boost_INCLUDE_DIRS}")
    endif()
  endif()

  if(NOT LOCKFREESTRUCTURES_BOOST_INCLUDE_DIR)
    message(
      FATAL_ERROR
      "Boost.Lockfree headers were not found. Install Boost 1.74+ or set BOOST_ROOT."
    )
  endif()

  add_executable(
    queue_benchmark
    "${PROJECT_SOURCE_DIR}/benchmark/queue_benchmark.cc"
  )

  target_include_directories(
    queue_benchmark
    SYSTEM
    PRIVATE
      "${LOCKFREESTRUCTURES_BOOST_INCLUDE_DIR}"
      "${PROJECT_SOURCE_DIR}/benchmark/third_party"
  )

  target_link_libraries(
    queue_benchmark
    PRIVATE
      LockFreeStructures::lockfree_structures
      Threads::Threads
      benchmark::benchmark
      TBB::tbb
  )

  lockfree_enable_warnings(queue_benchmark)

  set_target_properties(
    queue_benchmark
    PROPERTIES
      RUNTIME_OUTPUT_DIRECTORY "${PROJECT_BINARY_DIR}/benchmark/bin"
      RUNTIME_OUTPUT_DIRECTORY_DEBUG "${PROJECT_BINARY_DIR}/benchmark/bin"
      RUNTIME_OUTPUT_DIRECTORY_RELEASE "${PROJECT_BINARY_DIR}/benchmark/bin"
      RUNTIME_OUTPUT_DIRECTORY_RELWITHDEBINFO "${PROJECT_BINARY_DIR}/benchmark/bin"
      RUNTIME_OUTPUT_DIRECTORY_MINSIZEREL "${PROJECT_BINARY_DIR}/benchmark/bin"
  )

  if(WIN32)
    add_custom_command(
      TARGET queue_benchmark
      POST_BUILD
      COMMAND "${CMAKE_COMMAND}" -E copy_if_different
              "$<TARGET_FILE:TBB::tbb>"
              "$<TARGET_FILE_DIR:queue_benchmark>"
      VERBATIM
    )
  endif()
endif()
