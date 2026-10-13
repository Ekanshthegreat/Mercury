cmake_minimum_required(VERSION 3.20)

# Mercury's tests are deliberately dependency-light: no GoogleTest, no Catch2,
# no network fetch. A tiny assertion header plus CTest registration is enough
# for the invariant/property tests the implementation plan calls for, and it
# keeps a fresh clone buildable with nothing but a C++20 compiler and CMake.

add_library(mercury_test_main INTERFACE)
target_include_directories(mercury_test_main INTERFACE ${CMAKE_CURRENT_SOURCE_DIR})

function(mercury_add_test name)
    add_executable(${name} ${name}.cpp)
    target_link_libraries(${name} PRIVATE mercury_engine mercury_test_main)
    add_test(NAME ${name} COMMAND ${name})
endfunction()

mercury_add_test(test_order_book)
mercury_add_test(test_order_book_property)
mercury_add_test(test_simulation)

mercury_add_test(test_execution)

mercury_add_test(test_experiments)
