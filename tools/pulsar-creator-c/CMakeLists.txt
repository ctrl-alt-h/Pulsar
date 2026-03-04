cmake_minimum_required(VERSION 3.16)
project(pulsar_creator_c C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

add_executable(pulsar-creator main.c)

if (MSVC)
  target_compile_options(pulsar-creator PRIVATE /W4)
else()
  target_compile_options(pulsar-creator PRIVATE -Wall -Wextra -Wpedantic)
endif()
