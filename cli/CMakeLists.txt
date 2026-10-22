add_executable(blockwright-cli main.cpp)
add_dependencies(blockwright-cli bw_version)
target_include_directories(blockwright-cli PRIVATE ${CMAKE_BINARY_DIR}/generated)
target_link_libraries(blockwright-cli PRIVATE blockwright::core Qt6::Core)
target_compile_options(blockwright-cli PRIVATE
  $<$<CXX_COMPILER_ID:GNU,Clang>:-Wall -Wextra -Wpedantic>)
install(TARGETS blockwright-cli RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
