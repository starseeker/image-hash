cmake_minimum_required (VERSION 3.18)

project (imghash)

find_package(PNG REQUIRED)

add_executable (imghash main.cpp PImgHash.cpp imgio.cpp)
target_link_libraries(imghash PRIVATE PNG::PNG)
target_compile_definitions(imghash PRIVATE USE_PNG)

target_compile_features(imghash PUBLIC cxx_std_11)

