# cmake -DCHECK=<plural_strings_check.cmake> -DFIXTURES=<dir> -P plural_strings_fixtures.cmake
#
# Runs the plural_strings check on each fixture alone: a flag-*.cpp holds one
# message Qt would show as written and must fail with exactly that one, an
# ok-*.cpp must pass.
file(GLOB flags "${FIXTURES}/flag-*.cpp")
file(GLOB oks "${FIXTURES}/ok-*.cpp")
if(NOT flags OR NOT oks)
  message(FATAL_ERROR "no flag-*.cpp or ok-*.cpp fixtures in ${FIXTURES}")
endif()
set(failed 0)
foreach(f IN LISTS flags oks)
  get_filename_component(name "${f}" NAME)
  execute_process(COMMAND "${CMAKE_COMMAND}" -DFILE=${f} -P "${CHECK}"
                  RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE out)
  if(name MATCHES "^flag-")
    if(rc EQUAL 0 OR NOT out MATCHES "[^0-9]1 plural messages Qt shows as written")
      message("${name}: should be found once, got:\n${out}")
      math(EXPR failed "${failed} + 1")
    endif()
  elseif(NOT rc EQUAL 0)
    message("${name}: should pass, got:\n${out}")
    math(EXPR failed "${failed} + 1")
  endif()
endforeach()
if(failed)
  message(FATAL_ERROR "${failed} plural_strings fixtures went wrong")
endif()
list(LENGTH flags nf)
list(LENGTH oks no)
message("plural_strings: ${nf} flag fixtures found, ${no} ok fixtures pass")
