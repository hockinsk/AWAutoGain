cmake_minimum_required(VERSION 3.15)
cmake_policy(SET CMP0091 NEW)   # enable MSVC_RUNTIME_LIBRARY selection
project(awautogain LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Share one dynamic CRT across all wrapper copies (a host can load hundreds of
# them during a scan; static CRT in each would multiply TLS/footprint).
if (MSVC)
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreadedDLL")
endif()

# The reusable proxy shim (one binary, copied per-plugin by the generator).
add_library(awautogain MODULE src/shim.cpp)
set_target_properties(awautogain PROPERTIES PREFIX "")   # no "lib" prefix
if (WIN32)
    set_target_properties(awautogain PROPERTIES SUFFIX ".dll")
elseif (APPLE)
    set_target_properties(awautogain PROPERTIES SUFFIX ".vst")  # rename bundle as needed
else()
    set_target_properties(awautogain PROPERTIES SUFFIX ".so")
endif()
if (NOT WIN32)
    target_link_libraries(awautogain PRIVATE ${CMAKE_DL_LIBS})
endif()

# The generator CLI.
add_executable(airwrap src/airwrap.cpp)
