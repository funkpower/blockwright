#pragma once
// The cell width of a code point (PLAN §16, Width): glibc's wcwidth rules
// over a table generated from a pinned Unicode version by scripts/gen-width
// (core/src/width.inc), not Qt's or the host's tables, so the columns a file
// reads as don't depend on the build.

namespace bw {

// 0 for a zero-width code point (it joins the cluster before it), 1 or 2
// columns, or -1 for a control a cell refuses (U+0001-001F, U+007F, the C1
// controls, U+2028, U+2029), a surrogate or a value above U+10FFFF. U+0000,
// which a cell holds as a blank, is 1, as is every code point CP437 maps to.
int codePointWidth(char32_t cp);

// The Unicode version the table was generated from ("18.0.0").
const char* widthUnicodeVersion();

// Whether `cp` is zero-width or wide (codePointWidth 0 or 2) and a terminal
// or IRC client on an older table may give it another width: it was
// assigned after widthVariesFloor() (DerivedAge.txt), isn't assigned yet, or
// that version's rules give it another width (lint's width-varies).
bool widthVaries(char32_t cp);

// The oldest Unicode version widthVaries allows for ("9.0.0", which made
// emoji wide).
const char* widthVariesFloor();

} // namespace bw
