cmake_minimum_required(VERSION 3.16)
project(esimc C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# ---- Find LLVM ----
find_package(LLVM REQUIRED CONFIG)
message(STATUS "Found LLVM ${LLVM_PACKAGE_VERSION}")
message(STATUS "Using LLVMConfig.cmake in: ${LLVM_DIR}")

include_directories(${LLVM_INCLUDE_DIRS})
add_definitions(${LLVM_DEFINITIONS})
llvm_map_components_to_libnames(LLVM_LIBS core)

# ---- Find flex and bison ----
find_package(FLEX REQUIRED)
find_package(BISON 3.0 REQUIRED)

# ---- Generate parser and lexer ----
set(PARSER_OUT ${CMAKE_CURRENT_BINARY_DIR}/parser.tab.cpp)
set(PARSER_HDR ${CMAKE_CURRENT_BINARY_DIR}/parser.tab.hpp)
set(LEXER_OUT  ${CMAKE_CURRENT_BINARY_DIR}/lexer.cpp)

BISON_TARGET(Parser src/parser.y ${PARSER_OUT}
    DEFINES_FILE ${PARSER_HDR})
FLEX_TARGET(Lexer src/lexer.l ${LEXER_OUT})
ADD_FLEX_BISON_DEPENDENCY(Lexer Parser)

# ---- Compiler executable ----
add_executable(esimc
    src/main.cpp
    src/codegen.cpp
    ${PARSER_OUT}
    ${LEXER_OUT}
)

target_include_directories(esimc PRIVATE
    src
    ${CMAKE_CURRENT_BINARY_DIR}
)

target_link_libraries(esimc PRIVATE ${LLVM_LIBS})

# Suppress flex-generated code warnings
set_source_files_properties(${LEXER_OUT} PROPERTIES
    COMPILE_FLAGS "-Wno-deprecated-register -Wno-unused-function")

# ---- Runtime library ----
add_library(simula_rt STATIC runtime/simula_rt.c)
target_include_directories(simula_rt PUBLIC runtime)
