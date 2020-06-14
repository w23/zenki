cmake_minimum_required(VERSION 3.10)
project(zenki)

find_package(Threads REQUIRED)
find_package(PkgConfig)
pkg_check_modules(avformat REQUIRED IMPORTED_TARGET "libavformat")
pkg_check_modules(avutil REQUIRED IMPORTED_TARGET "libavutil")
pkg_check_modules(avcodec REQUIRED IMPORTED_TARGET "libavcodec")
pkg_check_modules(yaml REQUIRED IMPORTED_TARGET "yaml-0.1")

set(SOURCES
	src/main.c
	src/zcamera.c
)

add_executable(zenki ${SOURCES})
target_link_libraries(zenki PRIVATE
	Threads::Threads
	PkgConfig::avformat
	PkgConfig::avutil
	PkgConfig::avcodec
	PkgConfig::yaml
)
