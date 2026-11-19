Name:           blockwright
Version:        0.1.2
Release:        1%{?dist}
Summary:        Editor for ANSI and IRC block art

# GPL-3.0-or-later: the program; CC-BY-SA-4.0: the bundled VGA and BIOS
# glyph bitmaps; MIT: the bundled Tabler icons; OFL-1.1 OR GPL-2.0-or-later
# WITH Font-exception-2.0: the bundled GNU Unifont glyphs; OFL-1.1: the
# bundled Noto Sans
License:        GPL-3.0-or-later AND CC-BY-SA-4.0 AND MIT AND (OFL-1.1 OR GPL-2.0-or-later WITH Font-exception-2.0) AND OFL-1.1
URL:            https://git.pumpin.online/funkpower/blockwright
Source0:        %{url}/releases/download/v%{version}/%{name}-%{version}.tar.gz

BuildRequires:  cmake >= 3.24
BuildRequires:  desktop-file-utils
BuildRequires:  gcc-c++
BuildRequires:  libappstream-glib
BuildRequires:  ninja-build
# Qt 6.7 is the minimum (CMakeLists.txt, #263)
BuildRequires:  cmake(Qt6Core) >= 6.7
BuildRequires:  cmake(Qt6Concurrent)
BuildRequires:  cmake(Qt6Gui)
BuildRequires:  cmake(Qt6Svg)
BuildRequires:  cmake(Qt6Widgets)
BuildRequires:  cmake(libzip) >= 1.7
BuildRequires:  cmake(zstd)
%bcond_without check
%if %{with check}
BuildRequires:  cmake(Catch2) >= 3
BuildRequires:  python3
# QWindowSystemInterface, which the GUI tests deliver input through
BuildRequires:  qt6-qtbase-private-devel
%endif

# The renderer draws the glyphs CP437 lacks with GNU Unifont's bitmaps. Fedora
# ships only the BMP unifont.hex, without the upper planes (no unifont_upper or
# unifont_all .hex) that BLOCKWRIGHT_SYSTEM_UNIFONT also needs, so the copy in
# third_party/unifont/ is compiled in.
Provides:       bundled(unifont) = 18.0.01

Requires:       hicolor-icon-theme
Recommends:     python3
Recommends:     qt6-qtimageformats

%description
Blockwright is an editor for ANSI art in CP437 block characters and for
IRC art. It has half-block, shade and form brushes, layers, text faces and
styles, a library of stamps, and generator layers that run scripts.

It reads and writes .ans with SAUCE, XBIN and BIN, exports IRC text within a
line budget and checks a piece against lint rules. Its command-line program
renders, converts and checks pieces without a display.

%prep
%autosetup
# the bundled fonts' and icons' licences, under names that don't collide in
# %%license, which installs even with tsflags=nodocs
cp -p third_party/oldschool-pc-fonts/LICENSE LICENSE.oldschool-pc-fonts
cp -p third_party/unifont/OFL-1.1.txt LICENSE.unifont-OFL
cp -p third_party/unifont/COPYING LICENSE.unifont-GPL
cp -p third_party/tabler-icons/LICENSE LICENSE.tabler-icons
cp -p third_party/noto-sans/OFL.txt LICENSE.noto-sans

%build
# Builders have no network: the tests use the packaged Catch2 3
%cmake -G Ninja -DBW_FETCH_CATCH2=OFF %{!?with_check:-DBUILD_TESTING=OFF}
%cmake_build

%install
%cmake_install
# NOTICE says "see LICENSE" beside it; point that at the %%license copy
ln -sf ../../licenses/%{name}/LICENSE %{buildroot}%{_pkgdocdir}/LICENSE

%check
desktop-file-validate %{buildroot}%{_datadir}/applications/%{name}.desktop
appstream-util validate-relax --nonet %{buildroot}%{_metainfodir}/*.metainfo.xml
%if %{with check}
export QT_QPA_PLATFORM=offscreen
# mock builds have no network, and the nested-build tests hang there
%ctest -- -LE nested-build
%endif

%files
%license LICENSE NOTICE LICENSE.oldschool-pc-fonts LICENSE.unifont-OFL LICENSE.unifont-GPL LICENSE.tabler-icons LICENSE.noto-sans
# NOTICE, and the bundled fonts' and icons' licences beside it where NOTICE
# and --version say they are; the %%license copies are the ones nodocs keeps
%doc %{_pkgdocdir}/
%{_bindir}/blockwright
%{_bindir}/blockwright-cli
%{_datadir}/blockwright/
%{_datadir}/applications/%{name}.desktop
%{_datadir}/icons/hicolor/*/apps/%{name}.png
%{_metainfodir}/online.pumpin.funkpower.blockwright.metainfo.xml
%{_mandir}/man1/blockwright.1*
%{_mandir}/man1/blockwright-cli.1*

%changelog
* Thu Nov 19 2026 funkpower - 0.1.2-1
- Micron as a target for NomadNet pages: Format > Target, a NomadNet View dock, Snap to Micron Colours, and .mu files opened as documents
- Palettes: draw a document in its own palette, swap presets, fit and remap colours
- Library collections, in a Library dock and in blockwright-cli
- An offline help window with the manual and tutorials
- IRC output can be limited to the 16 mIRC colours
- Lint notes for what Micron can't write, and a new sample 10
- Layer > Add Generator Layer... adds the output of a generator program as a layer
- Removed: File > Publish to Library and Send to WeeChat; File > Export IRC Text and blockwright-cli export --irc stay
- The offline help bundle is now shipped in every package
- On macOS the Layers dock shows every row's visibility and Alpha Lock boxes, not only the first row's; the Layer Style dialog's effect list has the same fix
- Library fixes: a second path to the bundled library is refused, a library delete or remove can be restored from the trash, and the Library dock's Add Folder keeps non-ASCII file names on Windows
- Windows: the project format refuses a directory or junction swapped in for a file
- blockwright-cli replay and blockwright --screenshot get View > Guides, placeStamp and saveToLibrary steps
- A Flatpak manifest and bundle script

* Sat Nov 14 2026 funkpower - 0.1.1-1
- Export to Micron for NomadNet pages, in the app and blockwright-cli
- New samples 07, 08 and 09, and a new sample 02
- Test fixtures run under CMake 4.5

* Sat Oct 31 2026 funkpower - 0.1.0-1
- First Fedora package
