# The core library: cell model, palette, CP437, ANSI and SAUCE codecs.
# Plain C++20; it must never link QtWidgets.
add_library(blockwright_core STATIC
  src/ansi.cpp
  src/canvas.cpp
  src/cp437.cpp
  src/palette.cpp
  src/sauce.cpp
)
add_library(blockwright::core ALIAS blockwright_core)
target_include_directories(blockwright_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_compile_options(blockwright_core PRIVATE
  $<$<CXX_COMPILER_ID:GNU,Clang>:-Wall -Wextra -Wpedantic>)
