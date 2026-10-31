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
  face_picker.cpp
  face_picker.h
  font_scanner.cpp
  flow_row.cpp
  flow_row.h
  font_scanner.h
  generator_form.cpp
  generator_form.h
  generator_runner.cpp
  generator_runner.h
  glyph_bar.cpp
  glyph_bar.h
  help_text.cpp
  help_text.h
  irc_meter.cpp
  irc_meter.h
  document.h
  layer_style_dialog.cpp
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
  manual_dialog.cpp
  manual_dialog.h
  palette_widget.cpp
  palette_widget.h
  preferences.cpp
  preferences.h
  publish.cpp
  publish.h
  reference_scale.cpp
  reference_scale.h
  row_tree.h
  sauce_dialog.cpp
  sauce_dialog.h
  select_tool.cpp
  select_tool.h
  settings.cpp
  settings.h
  shortcut_settings.cpp
  shortcut_settings.h
  shortcuts_dialog.cpp
  shortcuts_dialog.h
  text_options.cpp
  text_options.h
  text_tool.cpp
  text_tool.h
  tool.cpp
  tool.h
  tool_icon.cpp
  tool_icon.h
  tool_illustration.cpp
  tool_illustration.h
  tool_options.cpp
  tool_options.h
  toolbox.cpp
  toolbox.h
  transform_options.cpp
  transform_options.h
  whats_this.cpp
  whats_this.h
)
set_target_properties(blockwright_app PROPERTIES AUTOMOC ON)
# The help text (help_text.h): a Markdown file a tool and a layer kind.
qt_add_resources(blockwright_app "help" PREFIX "/help" BASE help FILES
  help/layers/cells.md
  help/layers/generator.md
  help/layers/pixels.md
  help/layers/reference.md
  help/layers/text.md
  help/tools/clone.md
  help/tools/colorize.md
  help/tools/despeckle.md
  help/tools/ellipse.md
  help/tools/eyedropper.md
  help/tools/fill.md
  help/tools/form.md
  help/tools/glyph.md
  help/tools/gradient.md
  help/tools/halfBlock.md
  help/tools/keyboard.md
  help/tools/line.md
  help/tools/move.md
  help/tools/outline.md
  help/tools/rampShade.md
  help/tools/rectangle.md
  help/tools/rimLight.md
  help/tools/select.md
  help/tools/shade.md
  help/tools/smudge.md
  help/tools/soft.md
  help/tools/spray.md
  help/tools/text.md
  help/tools/texture.md
)
# The Tabler icons the toolbox uses for the generic tools (tool_icon.h).
set(BW_TABLER_ICONS ${PROJECT_SOURCE_DIR}/third_party/tabler-icons/icons)
qt_add_resources(blockwright_app "tool_icons" PREFIX "/tool-icons" BASE ${BW_TABLER_ICONS} FILES
  ${BW_TABLER_ICONS}/arrows-move.svg
  ${BW_TABLER_ICONS}/bucket-droplet.svg
  ${BW_TABLER_ICONS}/color-picker.svg
  ${BW_TABLER_ICONS}/keyboard.svg
  ${BW_TABLER_ICONS}/line.svg
  ${BW_TABLER_ICONS}/marquee-2.svg
  ${BW_TABLER_ICONS}/oval.svg
  ${BW_TABLER_ICONS}/rubber-stamp.svg
  ${BW_TABLER_ICONS}/spray.svg
  ${BW_TABLER_ICONS}/square.svg
  ${BW_TABLER_ICONS}/typography.svg
)
add_dependencies(blockwright_app bw_version)
target_include_directories(blockwright_app PUBLIC ${CMAKE_CURRENT_SOURCE_DIR} ${CMAKE_BINARY_DIR}/generated)
target_link_libraries(blockwright_app PUBLIC blockwright::core blockwright::render Qt6::Widgets Qt6::Concurrent Qt6::Svg)
target_compile_options(blockwright_app PRIVATE
  $<$<CXX_COMPILER_ID:GNU,Clang>:-Wall -Wextra -Wpedantic>)

add_executable(blockwright main.cpp)
target_link_libraries(blockwright PRIVATE blockwright_app)
target_compile_options(blockwright PRIVATE
  $<$<CXX_COMPILER_ID:GNU,Clang>:-Wall -Wextra -Wpedantic>)
install(TARGETS blockwright RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
