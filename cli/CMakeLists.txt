# Reading input files, shared with the tests so the size limit can be
# exercised with a small injected maximum.
add_library(blockwright_cli_io STATIC read_file.cpp)
target_include_directories(blockwright_cli_io PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_options(blockwright_cli_io PRIVATE
  $<$<CXX_COMPILER_ID:GNU,Clang>:-Wall -Wextra -Wpedantic>)

add_executable(blockwright-cli main.cpp)
add_dependencies(blockwright-cli bw_version)
target_include_directories(blockwright-cli PRIVATE ${CMAKE_BINARY_DIR}/generated)
target_link_libraries(blockwright-cli PRIVATE blockwright_cli_io blockwright::core blockwright::render Qt6::Core Qt6::Gui)
target_compile_options(blockwright-cli PRIVATE
  $<$<CXX_COMPILER_ID:GNU,Clang>:-Wall -Wextra -Wpedantic>)
install(TARGETS blockwright-cli RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
