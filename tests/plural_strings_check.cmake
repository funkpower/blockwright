# cmake -DDIR=<app dir> -P plural_strings_check.cmake
# cmake -DFILE=<source> -P plural_strings_check.cmake
#
# No message in the app counts through tr or QCoreApplication::translate or
# writes "(s)". The app installs no translator, and without one Qt only puts
# n in for %n, so "%n line(s)" shows as "1 line(s)" (#304): plural() in
# app/plural.h picks the English form instead. A count is any third argument
# to tr (after the source text and a disambiguation of nullptr, Q_NULLPTR, 0,
# NULL, {} or a string) or fourth to translate (after the context too). The
# source text may be any expression: a string, a raw string R"(...)", a name,
# a table entry (kMsgs[i]), a member (e.text, e->text) or a call
# (s.toUtf8().constData(), nested two deep). A counted call is flagged whatever
# its source text, since a non-literal one can't be checked for "(s)".
# A bare tr( or translate( is checked, and so is a qualified one (QObject::tr,
# QCoreApplication::translate) or one called through this->, qApp-> or
# instance()-> (qApp->translate(...), QCoreApplication::instance()->tr(...)).
# A member of another object (m.tr(...), m->tr(...), get_instance()->tr(...))
# is not, nor is a variable declared by that name with its type on the same
# line (QRect tr(l, 0, w, h)).
# A declaration with its type on the line before (QRect\n tr(l, 0, w, h)) or
# a type ending in > (std::tuple<int, int, int> tr(1, 0, 2)) is flagged, and so
# is a call to a variable or lambda named tr with 0 or nullptr second (the
# check can't tell it from QObject::tr without resolving names).
# A comment between the arguments hides the call from the check;
# so does a raw string with a delimiter (R"x(...)x"). A line that is only a //
# comment is left alone; a "(s)" in any other string literal, a trailing
# comment's included, is found. A character literal is one character, a run
# of non-ASCII bytes ('░' written out) or an escape, so a digit separator
# (1'000) or an apostrophe in a comment opens none.
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
# R"(...)": anything up to the first )" (a delimiter isn't matched)
set(raw "R\"\\(([^)]|\\)+[^)\"])*\\)+\"")
set(ws "[ \t\r\n]*")
# The literals of a line in order, so code between two of them is never taken
# for one: a raw string, a string, or a character literal (so '"' opens none).
set(strlit "\"([^\"\\\\\n]|\\\\.)*\"")
set(chrlit "'([^'\r\n\\\\]|[^ -~\t\r\n]+|\\\\.[^'\\\\\n]*)'")
# The calls are matched with each literal, or run of adjacent ones, written
# "", so a comma or parenthesis in one ends nothing. The source text is an
# expression up to the next comma or closing parenthesis outside a
# parenthesized group, the group nested at most two deep.
set(text "([^(),]|\\(([^()]|\\([^()]*\\))*\\))+")
set(disambig "${ws}(nullptr|Q_NULLPTR|0|NULL|[{]${ws}[}]|\"\")${ws}")
# a word and a space on the same line before tr( or translate( (a type, in a
# declaration)
set(before "[A-Za-z_][A-Za-z0-9_]*[ \t]+")
# what may come before the name: this->, qApp-> or instance()-> (checked), ->
# (a member, skipped), a type (skipped), or any other character
set(lead "(^|[^A-Za-z0-9_](this|qApp|instance${ws}\\(${ws}\\))${ws}->|->|${before}|[^A-Za-z0-9_])")
set(bad 0)
foreach(src IN LISTS sources)
  file(READ "${src}" source)
  string(REPLACE ";" "<semicolon>" source "${source}")
  get_filename_component(file "${src}" NAME)
  string(REGEX REPLACE "(^|\n)[ \t]*//[^\n]*" "\\1" code "${source}")
  string(REGEX REPLACE "${raw}|${strlit}|${chrlit}" "\"\"" masked "${code}")
  string(REGEX REPLACE "\"\"(${ws}\"\")+" "\"\"" masked "${masked}")
  string(REGEX MATCHALL "${lead}tr${ws}\\(${text},${disambig},"
         counted "${masked}")
  string(REGEX MATCHALL "${lead}translate${ws}\\(${text},${text},${disambig},"
         translated "${masked}")
  list(APPEND counted ${translated})
  foreach(call IN LISTS counted)
    # what comes before the name: a member or a declaration isn't a call
    string(REGEX MATCH "^(${lead}|)(tr|translate)${ws}\\(" head "${call}")
    set(pre "${CMAKE_MATCH_1}")
    string(LENGTH "${pre}" skip)
    string(SUBSTRING "${call}" ${skip} -1 call)
    string(STRIP "${pre}" pre)
    if(pre MATCHES "^(->|[.])$" OR (pre MATCHES "^[A-Za-z_][A-Za-z0-9_]*$"
       AND NOT pre MATCHES "^(return|co_return|co_yield|throw|else|do|case)$"))
      continue()
    endif()
    string(REGEX REPLACE "[ \t\r\n]+" " " call "${call}")
    string(REPLACE "\"\"" "\"...\"" call "${call}")
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
