Name:           blockwright
Version:        0.1.0
Release:        1%{?dist}
Summary:        Editor for ANSI and IRC block art

# GPL-3.0-or-later: the program; CC-BY-SA-4.0: the bundled VGA and BIOS
# glyph bitmaps; MIT: the bundled Tabler icons
License:        GPL-3.0-or-later AND CC-BY-SA-4.0 AND MIT
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
# the bundled font's and icons' licences, under names that don't collide in
# %%license, which installs even with tsflags=nodocs
cp -p third_party/oldschool-pc-fonts/LICENSE LICENSE.oldschool-pc-fonts
cp -p third_party/tabler-icons/LICENSE LICENSE.tabler-icons

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
%ctest
%endif

%files
%license LICENSE NOTICE LICENSE.oldschool-pc-fonts LICENSE.tabler-icons
# NOTICE, and the bundled font's and icons' licences beside it where NOTICE
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
* Sat Oct 31 2026 funkpower - 0.1.0-1
- First Fedora package
