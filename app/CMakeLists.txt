# The GUI: Qt 6 Widgets. The window and its parts live in a library so the
# tests can drive them without the executable.
add_library(blockwright_app STATIC
  canvas_view.cpp
  canvas_view.h
  device_grid.cpp
  device_grid.h
  document.cpp
  draw_tools.cpp
  draw_tools.h
  generator_form.cpp
  generator_form.h
  generator_runner.cpp
  generator_runner.h
  glyph_bar.cpp
  glyph_bar.h
  irc_meter.cpp
  irc_meter.h
  document.h
  layers_dock.cpp
  layers_dock.h
  library_dock.cpp
  library_dock.h
  lint_dock.cpp
  lint_dock.h
  main_window.cpp
  main_window.h
  main_window_docks.cpp
  main_window_edit.cpp
  main_window_format.cpp
  main_window_generators.cpp
  main_window_irc.cpp
  main_window_library.cpp
  main_window_project.cpp
  palette_widget.cpp
  palette_widget.h
  preferences.cpp
  preferences.h
  publish.cpp
  publish.h
  reference_scale.cpp
  reference_scale.h
  sauce_dialog.cpp
  sauce_dialog.h
  select_tool.cpp
  select_tool.h
  settings.cpp
  settings.h
  tool.cpp
  tool.h
  tool_options.cpp
  tool_options.h
  toolbox.cpp
  toolbox.h
)
set_target_properties(blockwright_app PROPERTIES AUTOMOC ON)
add_dependencies(blockwright_app bw_version)
target_include_directories(blockwright_app PUBLIC ${CMAKE_CURRENT_SOURCE_DIR} ${CMAKE_BINARY_DIR}/generated)
target_link_libraries(blockwright_app PUBLIC blockwright::core blockwright::render Qt6::Widgets Qt6::Concurrent)
target_compile_options(blockwright_app PRIVATE
  $<$<CXX_COMPILER_ID:GNU,Clang>:-Wall -Wextra -Wpedantic>)

add_executable(blockwright main.cpp)
target_link_libraries(blockwright PRIVATE blockwright_app)
target_compile_options(blockwright PRIVATE
  $<$<CXX_COMPILER_ID:GNU,Clang>:-Wall -Wextra -Wpedantic>)
install(TARGETS blockwright RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
