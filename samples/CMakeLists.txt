# The sample projects (PLAN §12): each samples/<name>/ is a directory project,
# installed as it is to <datadir>/blockwright/samples/<name>/, where the app's
# Help > Samples and the start screen find them (samples.h). They are
# written by bw-make-samples, not by the build:
# `ninja bw-make-samples && ./samples/bw-make-samples <source>/samples`.
set(BW_SAMPLE_NAMES 01-layers-showcase 02-text-style-logo)
foreach(name ${BW_SAMPLE_NAMES})
  install(DIRECTORY ${name} DESTINATION ${CMAKE_INSTALL_DATADIR}/blockwright/samples)
endforeach()

# The tool that writes them: run it yourself, as it runs the fog generator with
# python3.
add_executable(bw-make-samples EXCLUDE_FROM_ALL maker/make_samples.cpp)
target_link_libraries(bw-make-samples PRIVATE blockwright::core Qt6::Gui)
target_compile_definitions(bw-make-samples PRIVATE BW_MAKER_DIR="${CMAKE_CURRENT_SOURCE_DIR}/maker")
