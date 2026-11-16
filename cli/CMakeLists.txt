# Reading input files, writing outputs all or none, the shade pass and
# reading a generator's directory, shared with the window's screenshots and
# the tests so the size limit can be exercised with a small
# injected maximum and the shade compared with the editor's ramp brush.
add_library(blockwright_cli_io STATIC generators.cpp lint.cpp loss.cpp read_file.cpp replay.cpp save_all.cpp shade.cpp)
target_include_directories(blockwright_cli_io PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(blockwright_cli_io PUBLIC blockwright::core PRIVATE Qt6::Core)
target_compile_options(blockwright_cli_io PRIVATE
  $<$<CXX_COMPILER_ID:GNU,Clang>:-Wall -Wextra -Wpedantic>)

# The web build (#211) has no command line: only the library above, for the GUI.
if(EMSCRIPTEN)
  return()
endif()
add_executable(blockwright-cli main.cpp)
add_dependencies(blockwright-cli bw_version)
target_include_directories(blockwright-cli PRIVATE ${CMAKE_BINARY_DIR}/generated)
target_link_libraries(blockwright-cli PRIVATE blockwright_cli_io blockwright::core blockwright::render Qt6::Core Qt6::Gui)
target_compile_options(blockwright-cli PRIVATE
  $<$<CXX_COMPILER_ID:GNU,Clang>:-Wall -Wextra -Wpedantic>)
if(WIN32)
  bw_windows_resources(blockwright-cli "Blockwright command line")
endif()
install(TARGETS blockwright-cli RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
