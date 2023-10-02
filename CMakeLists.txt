cmake_minimum_required(VERSION 3.27)
project(liquidctl_energy)

set(CMAKE_CXX_STANDARD 20)

find_package(fmt REQUIRED)
find_package(simdjson REQUIRED)
add_subdirectory(argparse)
add_subdirectory(date)

add_executable(liquidctl_energy
	main.cpp
)
target_link_libraries(liquidctl_energy
	fmt::fmt
	argparse::argparse
	simdjson::simdjson
	date::date
)
