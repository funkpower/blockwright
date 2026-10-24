# The core library: cell model, palette, CP437, ANSI and SAUCE codecs, IRC
# text (mIRC colours and the row encoder), ramps, brushes and the lint.
# Plain C++20 (QtCore for the Library, tag case folding and projects' JSON;
# libzip and zstd for project files); it must never link QtWidgets.
add_library(blockwright_core STATIC
  src/ansi.cpp
  src/brush.cpp
  src/brushes/despeckle.cpp
  src/brushes/distance.cpp
  src/brushes/detail.cpp
  src/brushes/drawing.cpp
  src/brushes/form.cpp
  src/brushes/guard.cpp
  src/brushes/shading.cpp
  src/brushes/soft.cpp
  src/canvas.cpp
  src/cp437.cpp
  src/edit.cpp
  src/glyph_sets.cpp
  src/composite.cpp
  src/halfblock.cpp
  src/irc.cpp
  src/layer.cpp
  src/library.cpp
  src/lint.cpp
  src/mirc.cpp
  src/palette.cpp
  src/pipeline.cpp
  src/project.cpp
  src/ramp.cpp
  src/sauce.cpp
  src/selection.cpp
  src/shapes.cpp
)
add_library(blockwright::core ALIAS blockwright_core)
target_include_directories(blockwright_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
# The Library (library.cpp) reads JSON sidecars and finds the app data
# directory through QtCore, the pipeline (pipeline.cpp) lowercases tags with
# QString, and projects (project.cpp) read and write their JSON with it, which
# PLAN §4 allows in core. Projects are zip files (libzip) whose layer cells
# are zstd frames (found in the top-level CMakeLists).
target_link_libraries(blockwright_core PRIVATE Qt6::Core libzip::zip ${BW_ZSTD})
# Where the bundled library is installed, from the binaries' directory.
file(RELATIVE_PATH BW_LIBRARY_FROM_BINDIR ${CMAKE_INSTALL_FULL_BINDIR}
     ${CMAKE_INSTALL_FULL_DATADIR}/blockwright/library)
set_source_files_properties(src/library.cpp PROPERTIES
  COMPILE_DEFINITIONS "BW_LIBRARY_FROM_BINDIR=\"${BW_LIBRARY_FROM_BINDIR}\"")
target_compile_options(blockwright_core PRIVATE
  $<$<CXX_COMPILER_ID:GNU,Clang>:-Wall -Wextra -Wpedantic>)
