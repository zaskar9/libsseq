cmake_minimum_required(VERSION 3.20)
project(libsseq VERSION 0.2.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(SSEQ_BUILD_TOOLS "Build the sseqtool command-line tool" ON)
option(SSEQ_BUILD_TESTS "Build the tests" ON)

add_library(sseq
    src/model.cpp
    src/wrk.cpp
    src/smf.cpp
    src/engine.cpp
)
target_include_directories(sseq PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>
)
set(SSEQ_WARNINGS $<$<CXX_COMPILER_ID:GNU,Clang,AppleClang>:-Wall -Wextra -Wpedantic -Wshadow -Wold-style-cast -Wconversion -Wsign-conversion>)
target_compile_options(sseq PRIVATE ${SSEQ_WARNINGS})
add_library(sseq::sseq ALIAS sseq)

if(SSEQ_BUILD_TOOLS)
    add_executable(sseqtool tools/sseqtool.cpp)
    target_link_libraries(sseqtool PRIVATE sseq)
    target_compile_options(sseqtool PRIVATE ${SSEQ_WARNINGS})
endif()

if(SSEQ_BUILD_TESTS)
    enable_testing()
    add_executable(sseq_unit_tests tests/unit_tests.cpp)
    target_link_libraries(sseq_unit_tests PRIVATE sseq)
    target_compile_options(sseq_unit_tests PRIVATE ${SSEQ_WARNINGS})
    add_test(NAME unit COMMAND sseq_unit_tests)

    add_executable(sseq_engine_tests tests/engine_tests.cpp)
    target_link_libraries(sseq_engine_tests PRIVATE sseq_corpus)
    target_compile_options(sseq_engine_tests PRIVATE ${SSEQ_WARNINGS})
    add_test(NAME engine COMMAND sseq_engine_tests)

    # The corpus: song definitions shared by the generator and the corpus test.
    add_library(sseq_corpus STATIC tests/corpus/songs.cpp)
    target_link_libraries(sseq_corpus PUBLIC sseq)
    target_include_directories(sseq_corpus PUBLIC tests)
    target_compile_options(sseq_corpus PRIVATE ${SSEQ_WARNINGS})

    add_executable(gencorpus tests/corpus/gencorpus.cpp)
    target_link_libraries(gencorpus PRIVATE sseq_corpus)
    target_compile_options(gencorpus PRIVATE ${SSEQ_WARNINGS})

    add_executable(sseq_corpus_tests tests/corpus_tests.cpp)
    target_link_libraries(sseq_corpus_tests PRIVATE sseq_corpus)
    target_compile_options(sseq_corpus_tests PRIVATE ${SSEQ_WARNINGS})
    add_test(NAME corpus COMMAND sseq_corpus_tests "${CMAKE_CURRENT_SOURCE_DIR}/tests/data")
endif()

install(TARGETS sseq EXPORT sseqTargets)
install(DIRECTORY include/ DESTINATION include)
