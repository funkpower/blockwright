# The GUI: Qt 6 Widgets. The window and its parts live in a library so the
# tests can drive them without the executable.
add_library(blockwright_app STATIC
  canvas_view.cpp
  canvas_view.h
  document.cpp
  draw_tools.cpp
  draw_tools.h
  document.h
  main_window.cpp
  main_window.h
  main_window_edit.cpp
  main_window_format.cpp
  palette_widget.cpp
  palette_widget.h
  reference_scale.cpp
  reference_scale.h
  sauce_dialog.cpp
  sauce_dialog.h
  select_tool.cpp
  select_tool.h
  tool.cpp
  tool.h
)
set_target_properties(blockwright_app PROPERTIES AUTOMOC ON)
add_dependencies(blockwright_app bw_version)
target_include_directories(blockwright_app PUBLIC ${CMAKE_CURRENT_SOURCE_DIR} ${CMAKE_BINARY_DIR}/generated)
target_link_libraries(blockwright_app PUBLIC blockwright::core blockwright::render Qt6::Widgets)
target_compile_options(blockwright_app PRIVATE
  $<$<CXX_COMPILER_ID:GNU,Clang>:-Wall -Wextra -Wpedantic>)

add_executable(blockwright main.cpp)
target_link_libraries(blockwright PRIVATE blockwright_app)
target_compile_options(blockwright PRIVATE
  $<$<CXX_COMPILER_ID:GNU,Clang>:-Wall -Wextra -Wpedantic>)
install(TARGETS blockwright RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
