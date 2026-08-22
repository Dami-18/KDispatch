cmake_minimum_required(VERSION 3.20)
project(kdispatch CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()
set(CMAKE_CXX_FLAGS_RELEASE "-O2 -g")

find_package(Threads REQUIRED)

add_executable(server_userspace src/server_userspace.cpp)
add_executable(server_kcm       src/server_kcm.cpp)
add_executable(server_module    src/server_module.cpp)
add_executable(loadgen          src/loadgen.cpp)

foreach(tgt server_userspace server_kcm server_module loadgen)
  target_include_directories(${tgt} PRIVATE include)
  target_link_libraries(${tgt} PRIVATE Threads::Threads)
  target_compile_options(${tgt} PRIVATE -Wall -Wextra)
endforeach()
