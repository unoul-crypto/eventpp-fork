cmake_minimum_required(VERSION 3.13)

project(eventpptest)

set(CMAKE_CXX_STANDARD 11)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(EVENTPP_TEST_SANITIZER "none" CACHE STRING "Test instrumentation: none, address or thread")
set_property(CACHE EVENTPP_TEST_SANITIZER PROPERTY STRINGS none address thread)
if(NOT EVENTPP_TEST_SANITIZER MATCHES "^(none|address|thread)$")
	message(FATAL_ERROR "EVENTPP_TEST_SANITIZER must be none, address or thread")
endif()
if(NOT EVENTPP_TEST_SANITIZER STREQUAL "none")
	if(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux" OR NOT CMAKE_CXX_COMPILER_ID MATCHES "^(GNU|Clang)$")
		message(FATAL_ERROR "Test sanitizer builds require GCC or Clang on Linux")
	endif()
endif()

function(eventpp_configure_test target)
	if(NOT EVENTPP_TEST_SANITIZER STREQUAL "none")
		target_compile_options(${target} PRIVATE
			-fsanitize=${EVENTPP_TEST_SANITIZER} -fno-omit-frame-pointer -g)
		target_link_options(${target} PRIVATE -fsanitize=${EVENTPP_TEST_SANITIZER})
		target_compile_definitions(${target} PRIVATE EVENTPP_TEST_SANITIZED=1)
	endif()
endfunction()

set(THIRDPARTY_PATH ../../thirdparty)

include_directories(../include)

if(MSVC)
	add_definitions(/W4)
else()
	add_definitions(-Wall -Wextra -Wpedantic)
endif()

enable_testing()

add_subdirectory(tutorial)
add_subdirectory(unittest)
add_subdirectory(compatibility)
add_subdirectory(benchmark)
