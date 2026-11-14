# The sample projects (PLAN §12): each samples/<name>/ is a directory project,
# installed as it is to <datadir>/blockwright/samples/<name>/, where the app's
# Help > Samples and the start screen find them (samples.h). They are
# written by bw-make-samples, not by the build:
# `ninja bw-make-samples && ./samples/bw-make-samples <source>/samples`.
set(BW_SAMPLE_NAMES 01-layers-showcase 02-ddos-king 03-still-life 04-generator-ridges 05-character 06-blockwright-logo)
foreach(name ${BW_SAMPLE_NAMES})
  install(DIRECTORY ${name} DESTINATION ${CMAKE_INSTALL_DATADIR}/blockwright/samples)
endforeach()

# The tool that writes them: run it yourself, as it runs the generators (the fog's
# python3, the ridges' and stars' sh and awk). It is built with the tests on
# a POSIX host, where the samples_remade test runs it to check that the
# samples checked in are what it writes; otherwise it is outside the default
# build.
if(BUILD_TESTING AND UNIX AND NOT CMAKE_CROSSCOMPILING)
  set(bw_maker_all "")
else()
  set(bw_maker_all EXCLUDE_FROM_ALL)
endif()
add_executable(bw-make-samples ${bw_maker_all} maker/make_samples.cpp)
target_link_libraries(bw-make-samples PRIVATE blockwright::core)
target_compile_definitions(bw-make-samples PRIVATE BW_MAKER_DIR="${CMAKE_CURRENT_SOURCE_DIR}/maker")
