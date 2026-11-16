# The GUI: Qt 6 Widgets. The window and its parts live in a library so the
# tests can drive them without the executable.
add_library(blockwright_app STATIC
  action_list.cpp
  action_list.h
  branding.cpp
  branding.h
  canvas_view.cpp
  canvas_view.h
  device_grid.cpp
  device_grid.h
  document.cpp
  draw_tools.cpp
  draw_tools.h
  face_picker.cpp
  face_picker.h
  file_open.cpp
  file_open.h
  font_scanner.cpp
  flow_row.cpp
  flow_row.h
  frame_style_dialog.cpp
  frame_tool.cpp
  frame_tool.h
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
  item_check_style.cpp
  item_check_style.h
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
  main_window_samples.cpp
  manual_dialog.cpp
  manual_dialog.h
  message_box.cpp
  message_box.h
  micron_colour_dialog.cpp
  micron_colour_dialog.h
  palette_widget.cpp
  palette_widget.h
  plural.h
  preferences.cpp
  preferences.h
  recent_files.cpp
  recent_files.h
  reference_scale.cpp
  reference_scale.h
  row_tree.h
  canvas_size_dialog.cpp
  canvas_size_dialog.h
  sauce_dialog.cpp
  sauce_dialog.h
  screenshot.cpp
  screenshot.h
  select_tool.cpp
  select_tool.h
  settings.cpp
  settings.h
  shortcut_settings.cpp
  shortcut_settings.h
  shortcuts_dialog.cpp
  shortcuts_dialog.h
  start_screen.cpp
  start_screen.h
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
  help/tools/frame.md
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
# The UI font of blockwright --screenshot (PLAN §9, #295): Noto Sans,
# regular and bold, at :/ui-font/
set(BW_NOTO_SANS ${PROJECT_SOURCE_DIR}/third_party/noto-sans)
qt_add_resources(blockwright_app "ui_font" PREFIX "/ui-font" BASE ${BW_NOTO_SANS} FILES
  ${BW_NOTO_SANS}/NotoSans-Regular.ttf
  ${BW_NOTO_SANS}/NotoSans-Bold.ttf
)
# The logos and the app icon (#205): :/branding/logo-large.ans (the start
# screen's), :/branding/logo-small.ans (the About box's) and
# :/branding/blockwright-<size>.png, the icon branding/render writes to
# packaging/linux/
set(BW_BRANDING ${PROJECT_SOURCE_DIR}/branding)
qt_add_resources(blockwright_app "branding_logos" PREFIX "/branding" BASE ${BW_BRANDING} FILES
  ${BW_BRANDING}/logo-large.ans
  ${BW_BRANDING}/logo-small.ans
)
set(BW_ICON_PNGS ${PROJECT_SOURCE_DIR}/packaging/linux)
qt_add_resources(blockwright_app "branding_icon" PREFIX "/branding" BASE ${BW_ICON_PNGS} FILES
  ${BW_ICON_PNGS}/blockwright-16.png
  ${BW_ICON_PNGS}/blockwright-32.png
  ${BW_ICON_PNGS}/blockwright-48.png
  ${BW_ICON_PNGS}/blockwright-128.png
  ${BW_ICON_PNGS}/blockwright-256.png
  ${BW_ICON_PNGS}/blockwright-512.png
)
add_dependencies(blockwright_app bw_version)
target_include_directories(blockwright_app PUBLIC ${CMAKE_CURRENT_SOURCE_DIR} ${CMAKE_BINARY_DIR}/generated)
# blockwright_cli_io for the replay --screenshot shares with blockwright-cli (#295)
target_link_libraries(blockwright_app PUBLIC blockwright::core blockwright::render blockwright_cli_io
                      Qt6::Widgets Qt6::Concurrent Qt6::Svg)
target_compile_options(blockwright_app PRIVATE
  $<$<CXX_COMPILER_ID:GNU,Clang>:-Wall -Wextra -Wpedantic>)

# The web build (#211, PLAN §18) needs Qt's own executable, which adds the
# Emscripten link options and writes blockwright.html and qtloader.js.
if(EMSCRIPTEN)
  qt_add_executable(blockwright main.cpp)
else()
  add_executable(blockwright main.cpp)
endif()
if(WIN32)
  # a GUI program: no console window opens with it (main.cpp writes
  # --version and --help to the one it was started from)
  set_target_properties(blockwright PROPERTIES WIN32_EXECUTABLE ON)
  bw_windows_resources(blockwright "Blockwright, an ANSI and IRC art editor"
                       ${PROJECT_SOURCE_DIR}/packaging/windows/blockwright.ico)
endif()
target_link_libraries(blockwright PRIVATE blockwright_app)
target_compile_options(blockwright PRIVATE
  $<$<CXX_COMPILER_ID:GNU,Clang>:-Wall -Wextra -Wpedantic>)
if(BLOCKWRIGHT_MACOS_BUNDLE)
  # Blockwright.app/Contents/MacOS/Blockwright, its icon made from the PNG
  # packaging/macos/make-art.py draws
  set(bw_icns ${CMAKE_CURRENT_BINARY_DIR}/blockwright.icns)
  add_custom_command(OUTPUT ${bw_icns}
    COMMAND ${PROJECT_SOURCE_DIR}/packaging/macos/make-icns ${PROJECT_SOURCE_DIR}/packaging/macos/icon-1024.png ${bw_icns}
    DEPENDS ${PROJECT_SOURCE_DIR}/packaging/macos/icon-1024.png ${PROJECT_SOURCE_DIR}/packaging/macos/make-icns
    VERBATIM)
  target_sources(blockwright PRIVATE ${bw_icns})
  set_source_files_properties(${bw_icns} PROPERTIES MACOSX_PACKAGE_LOCATION Resources)
  set_target_properties(blockwright PROPERTIES
    OUTPUT_NAME Blockwright
    MACOSX_BUNDLE ON
    MACOSX_BUNDLE_INFO_PLIST ${PROJECT_SOURCE_DIR}/packaging/macos/Info.plist.in)
endif()
install(TARGETS blockwright RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR} BUNDLE DESTINATION .)
