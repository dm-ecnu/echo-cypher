add_executable(perquery perquery.cpp)
target_link_libraries(perquery PRIVATE ${ECHO_CORE_LIBS})
add_executable(echo_main echo_main.cpp)
target_link_libraries(echo_main PRIVATE ${ECHO_CORE_LIBS})

add_executable(calibrate_n3 calibrate_n3.cpp)
target_link_libraries(calibrate_n3 PRIVATE echo_core)
