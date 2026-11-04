# The tool that draws the generic stamps of the bundled Library (PLAN §12),
# outside the default build: `ninja bw-make-stamps &&
# ./library-maker/bw-make-stamps <source>/library`. Its output is checked in
# under library/; the build only installs it.
add_executable(bw-make-stamps EXCLUDE_FROM_ALL make_stamps.cpp)
target_link_libraries(bw-make-stamps PRIVATE blockwright::core)
