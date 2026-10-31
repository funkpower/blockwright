#pragma once
// The bundled fonts' attribution (CC BY-SA 4.0 section 3), printed by every
// binary's --version. They name where the notices are installed:
// share/doc/blockwright/, or inside the macOS app bundle (BW_NOTICES_DIR, set
// with BLOCKWRIGHT_MACOS_BUNDLE).
#ifndef BW_NOTICES_DIR
#define BW_NOTICES_DIR "share/doc/blockwright/"
#endif

namespace bw {

inline constexpr const char* kFontCredit =
    "Includes the IBM VGA 8x16 and IBM PC BIOS 8x8 fonts, (c) 2016-2020 VileR,\n"
    "CC BY-SA 4.0, https://int10h.org/oldschool-pc-fonts/; notices in " BW_NOTICES_DIR "\n";

// The toolbox icons' attribution (MIT), printed by the GUI's --version and
// shown in its About box: only the GUI ships them.
inline constexpr const char* kIconCredit =
    "Includes icons from Tabler Icons, MIT licence, https://tabler.io/icons; notices in " BW_NOTICES_DIR "\n";

} // namespace bw
