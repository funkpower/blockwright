# The core library: cell model, palette, CP437, ANSI and SAUCE codecs, IRC
# text through the linked ans2irc encoder, ramps, brushes and the lint.
# Plain C++20; it must never link QtWidgets.
add_library(blockwright_core STATIC
  src/ansi.cpp
  src/brush.cpp
  src/brushes/drawing.cpp
  src/brushes/shading.cpp
  src/canvas.cpp
  src/cp437.cpp
  src/edit.cpp
  src/glyph_sets.cpp
  src/halfblock.cpp
  src/irc.cpp
  src/lint.cpp
  src/palette.cpp
  src/ramp.cpp
  src/sauce.cpp
  src/selection.cpp
  src/shapes.cpp
)
add_library(blockwright::core ALIAS blockwright_core)
target_include_directories(blockwright_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(blockwright_core PRIVATE ans2irc_ffi)
target_compile_options(blockwright_core PRIVATE
  $<$<CXX_COMPILER_ID:GNU,Clang>:-Wall -Wextra -Wpedantic>)
