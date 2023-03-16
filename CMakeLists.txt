cmake_minimum_required (VERSION 3.18)

project (imghash)

find_package(PNG)

add_executable (imghash main.cpp imghash.cpp)

if(PNG_FOUND)
target_sources(imghash PUBLIC png.cpp)
target_link_libraries(imghash PRIVATE PNG::PNG)
target_compile_definitions(imghash PRIVATE USE_PNG)
set(features "${features} PNG")
endif()

target_compile_features(imghash PUBLIC cxx_std_17)

