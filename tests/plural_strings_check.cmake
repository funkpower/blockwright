# cmake -DDIR=<app dir> -P plural_strings_check.cmake
# cmake -DFILE=<source> -P plural_strings_check.cmake
#
# No message in the app counts through tr or QCoreApplication::translate or
# writes "(s)". The app installs no translator, and without one Qt only puts
# n in for %n, so "%n line(s)" shows as "1 line(s)" (#304): plural() in
# app/plural.h picks the English form instead. A count is any third argument
# to tr (after the source text and a disambiguation of nullptr, Q_NULLPTR, 0,
# NULL, {} or a string) or fourth to translate (after the context too). The
# source text may be a string, a raw string R"(...)" or a name (a QT_TR_NOOP
# table's entry, say). A comment between the arguments hides the call from
# the check; so does a raw string with a delimiter (R"x(...)x"). A line that
# is only a // comment is left alone; a "(s)" in any other string literal, a
# trailing comment's included, is found.
if(FILE)
  set(sources "${FILE}")
elseif(DIR)
  file(GLOB sources "${DIR}/*.cpp" "${DIR}/*.h")
else()
  message(FATAL_ERROR "usage: cmake -DDIR=<app dir> | -DFILE=<source> -P plural_strings_check.cmake")
endif()
if(NOT sources)
  message(FATAL_ERROR "no sources in ${DIR}")
endif()
set(lit "\"([^\"\\\\]|\\\\.)*\"")
# R"(...)": anything up to the first )" (a delimiter isn't matched)
set(raw "R\"\\(([^)]|\\)[^\"])*\\)\"")
set(ws "[ \t\r\n]*")
set(lits "(${ws}(${raw}|${lit}))+${ws}")
set(name "${ws}[A-Za-z_][A-Za-z0-9_:]*${ws}")
set(text "(${lits}|${name})")
set(disambig "${ws}(nullptr|Q_NULLPTR|0|NULL|[{]${ws}[}]|${lits})${ws}")
# The literals of a line in order, so code between two of them is never taken
# for one: a raw string, a string, or a character literal (so '"' opens none).
set(strlit "\"([^\"\\\\\n]|\\\\.)*\"")
set(chrlit "'([^'\\\\\n]|\\\\.)*'")
set(bad 0)
foreach(src IN LISTS sources)
  file(READ "${src}" source)
  string(REPLACE ";" "<semicolon>" source "${source}")
  get_filename_component(file "${src}" NAME)
  string(REGEX REPLACE "(^|\n)[ \t]*//[^\n]*" "\\1" code "${source}")
  string(REGEX MATCHALL "(^|[^A-Za-z0-9_])(tr${ws}\\(${text},${disambig},|translate${ws}\\(${text},${text},${disambig},)"
         counted "${code}")
  foreach(call IN LISTS counted)
    string(REGEX REPLACE "^[^A-Za-z0-9_]" "" call "${call}")
    string(REGEX REPLACE "[ \t\r\n]+" " " call "${call}")
    message("${file}: a count passed to ${call}")
    math(EXPR bad "${bad} + 1")
  endforeach()
  string(REGEX MATCHALL "${raw}|${strlit}|${chrlit}" literals "${code}")
  foreach(s IN LISTS literals)
    if(s MATCHES "^[R\"]" AND s MATCHES "\\(s\\)")
      message("${file}: \"(s)\" in ${s}")
      math(EXPR bad "${bad} + 1")
    endif()
  endforeach()
endforeach()
if(bad)
  message(FATAL_ERROR "${bad} plural messages Qt shows as written; use plural()")
endif()
message("no message counts through tr or writes (s)")
