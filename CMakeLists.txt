cmake_minimum_required(VERSION 3.20)

project(lobforge VERSION 0.1.0 LANGUAGES CXX)

option(LOB_BUILD_TESTS "Build the correctness test suite" ON)
option(LOB_BUILD_BENCHMARKS "Build the reproducible benchmark" ON)
option(LOB_ENABLE_SANITIZERS "Enable AddressSanitizer and UndefinedBehaviorSanitizer" OFF)
option(LOB_BUILD_FUZZING "Build the Clang/libFuzzer ITCH framing target" OFF)

add_library(lob STATIC src/order_book.cpp)
add_library(lob::lob ALIAS lob)
target_include_directories(lob PUBLIC "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>")
target_compile_features(lob PUBLIC cxx_std_20)

if(MSVC)
  target_compile_options(lob PRIVATE /W4 /permissive- /EHsc)
else()
  target_compile_options(lob PRIVATE -Wall -Wextra -Wpedantic -Wconversion -Wshadow)
endif()

add_library(lob_itch STATIC
  src/itch/messages.cpp
  src/itch/decoder.cpp
  src/itch/framed_reader.cpp
  src/replay/factual_book.cpp
  src/replay/book_event_v1.cpp
  src/replay/replayer.cpp
)
add_library(lob::itch ALIAS lob_itch)
target_include_directories(lob_itch PUBLIC "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>")
target_compile_features(lob_itch PUBLIC cxx_std_20)
if(MSVC)
  target_compile_options(lob_itch PRIVATE /W4 /permissive- /EHsc)
else()
  target_compile_options(lob_itch PRIVATE -Wall -Wextra -Wpedantic -Wconversion -Wshadow)
endif()
if(LOB_ENABLE_SANITIZERS)
  if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(lob_itch PUBLIC -fno-omit-frame-pointer -fsanitize=address,undefined)
    target_link_options(lob_itch PUBLIC -fno-omit-frame-pointer -fsanitize=address,undefined)
  endif()
endif()

add_executable(lobforge_replay apps/lobforge_replay.cpp)
target_link_libraries(lobforge_replay PRIVATE lob::itch)
target_compile_features(lobforge_replay PRIVATE cxx_std_20)
if(MSVC)
  target_compile_options(lobforge_replay PRIVATE /W4 /permissive- /EHsc)
else()
  target_compile_options(lobforge_replay PRIVATE -Wall -Wextra -Wpedantic -Wconversion -Wshadow)
endif()

if(LOB_BUILD_FUZZING)
  if(NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    message(FATAL_ERROR "LOB_BUILD_FUZZING requires Clang/libFuzzer")
  endif()
  add_executable(itch_fuzz fuzz/itch_fuzz.cpp)
  target_link_libraries(itch_fuzz PRIVATE lob::itch)
  target_compile_features(itch_fuzz PRIVATE cxx_std_20)
  target_compile_options(itch_fuzz PRIVATE -fno-omit-frame-pointer -fsanitize=fuzzer,address,undefined)
  target_link_options(itch_fuzz PRIVATE -fno-omit-frame-pointer -fsanitize=fuzzer,address,undefined)
endif()

if(LOB_ENABLE_SANITIZERS)
  if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(lob PUBLIC -fno-omit-frame-pointer -fsanitize=address,undefined)
    target_link_options(lob PUBLIC -fno-omit-frame-pointer -fsanitize=address,undefined)
  elseif(MSVC)
    message(FATAL_ERROR "LOB_ENABLE_SANITIZERS requires GCC or Clang; MSVC UBSan is unavailable")
  else()
    message(FATAL_ERROR "LOB_ENABLE_SANITIZERS is unsupported by ${CMAKE_CXX_COMPILER_ID}")
  endif()
endif()

if(LOB_BUILD_TESTS)
  enable_testing()
  add_executable(order_book_tests
    tests/order_book_tests.cpp
    tests/reference_order_book.cpp
  )
  target_link_libraries(order_book_tests PRIVATE lob::lob)
  target_include_directories(order_book_tests PRIVATE tests)
  target_compile_features(order_book_tests PRIVATE cxx_std_20)
  if(MSVC)
    target_compile_options(order_book_tests PRIVATE /W4 /permissive- /EHsc)
  else()
    target_compile_options(order_book_tests PRIVATE -Wall -Wextra -Wpedantic -Wconversion -Wshadow)
  endif()
  add_test(NAME order_book_tests COMMAND order_book_tests)

  add_executable(itch_replay_tests tests/itch_replay_tests.cpp)
  target_link_libraries(itch_replay_tests PRIVATE lob::itch)
  target_compile_features(itch_replay_tests PRIVATE cxx_std_20)
  target_compile_definitions(itch_replay_tests PRIVATE LOB_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}")
  if(MSVC)
    target_compile_options(itch_replay_tests PRIVATE /W4 /permissive- /EHsc)
  else()
    target_compile_options(itch_replay_tests PRIVATE -Wall -Wextra -Wpedantic -Wconversion -Wshadow)
  endif()
  add_test(NAME itch_replay_tests COMMAND itch_replay_tests)

  set(LOB_SYNTHETIC_FIXTURE "${CMAKE_CURRENT_BINARY_DIR}/synthetic_full_session.itch")
  set(LOB_FUZZ_CORPUS "${CMAKE_CURRENT_BINARY_DIR}/fuzz-corpus")
  add_test(NAME generate_round2_fixtures
    COMMAND itch_replay_tests --write-full-session "${LOB_SYNTHETIC_FIXTURE}")
  set_tests_properties(generate_round2_fixtures PROPERTIES FIXTURES_SETUP round2_fixture)
  add_test(NAME generate_fuzz_corpus
    COMMAND itch_replay_tests --write-fuzz-corpus "${LOB_FUZZ_CORPUS}")
  set_tests_properties(generate_fuzz_corpus PROPERTIES FIXTURES_SETUP fuzz_corpus)
  add_test(NAME replay_cli_text
    COMMAND lobforge_replay --input "${LOB_SYNTHETIC_FIXTURE}" --strict --symbol AAPL --top 10
            --format text)
  set_tests_properties(replay_cli_text PROPERTIES FIXTURES_REQUIRED round2_fixture)
  add_test(NAME replay_cli_book_event_v1_golden
    COMMAND ${CMAKE_COMMAND} -DPROGRAM=$<TARGET_FILE:lobforge_replay>
            -DINPUT=${LOB_SYNTHETIC_FIXTURE}
            -DEXPECTED=${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/book_event_v1_golden.ndjson
            -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/check_book_event_cli.cmake)
  set_tests_properties(replay_cli_book_event_v1_golden PROPERTIES FIXTURES_REQUIRED round2_fixture)
  add_test(NAME replay_cli_usage_exit
    COMMAND ${CMAKE_COMMAND} -DPROGRAM=$<TARGET_FILE:lobforge_replay> -DEXPECTED=2 -DCASE=usage
            -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/check_cli_exit.cmake)
  add_test(NAME replay_cli_io_exit
    COMMAND ${CMAKE_COMMAND} -DPROGRAM=$<TARGET_FILE:lobforge_replay> -DEXPECTED=3 -DCASE=io
            -DINPUT=${CMAKE_CURRENT_BINARY_DIR}/does-not-exist.itch
            -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/check_cli_exit.cmake)
  add_test(NAME replay_cli_strict_exit
    COMMAND ${CMAKE_COMMAND} -DPROGRAM=$<TARGET_FILE:lobforge_replay> -DEXPECTED=4 -DCASE=strict
            -DINPUT=${LOB_FUZZ_CORPUS}/malformed_3
            -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/check_cli_exit.cmake)
  set_tests_properties(replay_cli_strict_exit PROPERTIES FIXTURES_REQUIRED fuzz_corpus)
  add_test(NAME replay_cli_research_usage_exit
    COMMAND ${CMAKE_COMMAND} -DPROGRAM=$<TARGET_FILE:lobforge_replay> -DEXPECTED=2
            -DCASE=research_usage -DINPUT=${LOB_SYNTHETIC_FIXTURE}
            -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/check_cli_exit.cmake)
  set_tests_properties(replay_cli_research_usage_exit PROPERTIES FIXTURES_REQUIRED round2_fixture)

  find_package(Python3 COMPONENTS Interpreter QUIET)
  if(Python3_Interpreter_FOUND)
    add_test(NAME replay_cli_json_valid
      COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/validate_cli_json.py
              $<TARGET_FILE:lobforge_replay> "${LOB_SYNTHETIC_FIXTURE}")
    set_tests_properties(replay_cli_json_valid PROPERTIES FIXTURES_REQUIRED round2_fixture)
  endif()
endif()

if(LOB_BUILD_BENCHMARKS)
  add_executable(order_book_benchmark benchmarks/order_book_benchmark.cpp)
  target_link_libraries(order_book_benchmark PRIVATE lob::lob)
  target_compile_features(order_book_benchmark PRIVATE cxx_std_20)
  target_compile_definitions(order_book_benchmark PRIVATE
    LOB_COMPILER_FLAGS="${CMAKE_CXX_FLAGS} ${CMAKE_CXX_FLAGS_RELEASE}")
  if(MSVC)
    target_compile_options(order_book_benchmark PRIVATE /W4 /permissive- /EHsc)
  else()
    target_compile_options(order_book_benchmark PRIVATE -Wall -Wextra -Wpedantic -Wconversion -Wshadow)
  endif()

  add_executable(itch_replay_benchmark benchmarks/itch_replay_benchmark.cpp)
  target_link_libraries(itch_replay_benchmark PRIVATE lob::itch)
  target_compile_features(itch_replay_benchmark PRIVATE cxx_std_20)
  target_compile_definitions(itch_replay_benchmark PRIVATE
    LOB_COMPILER_FLAGS="${CMAKE_CXX_FLAGS} ${CMAKE_CXX_FLAGS_RELEASE}")
  if(MSVC)
    target_compile_options(itch_replay_benchmark PRIVATE /W4 /permissive- /EHsc)
  else()
    target_compile_options(itch_replay_benchmark PRIVATE -Wall -Wextra -Wpedantic -Wconversion -Wshadow)
  endif()
endif()

add_custom_target(format-check
  COMMAND clang-format --dry-run --Werror
    ${CMAKE_CURRENT_SOURCE_DIR}/include/lob/types.hpp
    ${CMAKE_CURRENT_SOURCE_DIR}/include/lob/commands.hpp
    ${CMAKE_CURRENT_SOURCE_DIR}/include/lob/events.hpp
    ${CMAKE_CURRENT_SOURCE_DIR}/include/lob/order_book.hpp
    ${CMAKE_CURRENT_SOURCE_DIR}/include/lob/itch/byte_reader.hpp
    ${CMAKE_CURRENT_SOURCE_DIR}/include/lob/itch/decoder.hpp
    ${CMAKE_CURRENT_SOURCE_DIR}/include/lob/itch/framed_reader.hpp
    ${CMAKE_CURRENT_SOURCE_DIR}/include/lob/itch/messages.hpp
    ${CMAKE_CURRENT_SOURCE_DIR}/include/lob/replay/factual_book.hpp
    ${CMAKE_CURRENT_SOURCE_DIR}/include/lob/replay/book_event_v1.hpp
    ${CMAKE_CURRENT_SOURCE_DIR}/include/lob/replay/replayer.hpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/order_book.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/itch/decoder.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/itch/framed_reader.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/itch/messages.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/replay/factual_book.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/replay/book_event_v1.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/replay/replayer.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/tests/reference_order_book.hpp
    ${CMAKE_CURRENT_SOURCE_DIR}/tests/reference_order_book.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/tests/order_book_tests.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/tests/itch_replay_tests.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/benchmarks/order_book_benchmark.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/benchmarks/itch_replay_benchmark.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/apps/lobforge_replay.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/fuzz/itch_fuzz.cpp
  WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
  COMMENT "Checking C++ formatting"
)
