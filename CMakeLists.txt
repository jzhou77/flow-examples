cmake_minimum_required(VERSION 3.12)

set(EXECUTABLE_OUTPUT_PATH ${PROJECT_BINARY_DIR}/bin)
set(LIBRARY_OUTPUT_PATH ${PROJECT_BINARY_DIR}/lib)
set(CMAKE_MODULE_PATH ${CMAKE_MODULE_PATH} "${CMAKE_CURRENT_SOURCE_DIR}/cmake/")

################################################################################
# Flow
################################################################################

# Flow and other tools are written in C# - so we need that dependency
# Install C# compiler on Ubuntu:
#    sudo apt install mono-mcs libmono-system-data4.0-cil libmono-system-xml-linq4.0-cil libmono-system-data-datasetextensions4.0-cil
include(EnableCsharp)

# First thing we need is the actor compiler - and to compile and run the
# actor compiler, we need mono
include(CompileActorCompiler)

#include(CompileCoverageTool)

# with the actor compiler, we can now make the flow commands available
include(FlowCommands)


include_directories(${CMAKE_SOURCE_DIR})
include_directories(${CMAKE_CURRENT_BINARY_DIR})
add_definitions(-DNO_INTELLISENSE)
add_definitions(-DUSE_UCONTEXT)


add_compile_definitions(BOOST_ERROR_CODE_HEADER_ONLY BOOST_SYSTEM_NO_DEPRECATED)

# Instruction sets we require to be supported by the CPU
add_compile_options(
  -maes
  -mmmx
  -mavx
  -msse4.2)
add_compile_options($<$<COMPILE_LANGUAGE:CXX>:-std=c++17>)
add_compile_options($<$<BOOL:${GCC}>:-Wno-pragmas>)
add_compile_options(-Wno-error=format
  -Wunused-variable
  -Wno-deprecated
  -fvisibility=hidden
  -Wreturn-type
  -fdiagnostics-color=always
  -fPIC)

include(CompileBoost)
find_package(Threads REQUIRED)

add_subdirectory(flow)
add_subdirectory(examples)

