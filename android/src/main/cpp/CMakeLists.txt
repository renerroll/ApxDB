cmake_minimum_required(VERSION 3.10)
project(apxdb)

add_library(apxdb SHARED apxdb.c)
set_target_properties(apxdb PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED ON)

find_library(log-lib log)
find_library(vulkan-lib vulkan)

target_link_libraries(apxdb ${log-lib} ${vulkan-lib})
