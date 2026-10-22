# The renderer: documents to images with the bundled VGA font. QtGui only;
# it must never link QtWidgets.
add_library(blockwright_render STATIC
  src/canvas_renderer.cpp
  src/render.cpp
)
add_library(blockwright::render ALIAS blockwright_render)
target_include_directories(blockwright_render PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(blockwright_render PUBLIC blockwright::core Qt6::Gui)
target_compile_options(blockwright_render PRIVATE
  $<$<CXX_COMPILER_ID:GNU,Clang>:-Wall -Wextra -Wpedantic>)
