# Windows resources (PLAN §14): every program carries
# packaging/windows/blockwright.manifest, which makes UTF-8 the process code
# page (a narrow path, std::filesystem's and the C runtime's, is UTF-8 as on
# Linux and macOS) and asks for long paths, and the two shipped programs a
# version resource from the project version.

set(BW_WINDOWS_MANIFEST ${PROJECT_SOURCE_DIR}/packaging/windows/blockwright.manifest)

# bw_windows_resources(<target> [<description>]): the manifest, and with a
# description a version resource naming the target's output file.
function(bw_windows_resources target)
  set(rc ${CMAKE_CURRENT_BINARY_DIR}/${target}.rc)
  file(TO_CMAKE_PATH "${BW_WINDOWS_MANIFEST}" manifest)
  set(text "#include <winver.h>\n1 24 \"${manifest}\"\n")
  if(ARGC GREATER 1)
    set(v ${PROJECT_VERSION_MAJOR},${PROJECT_VERSION_MINOR},${PROJECT_VERSION_PATCH},0)
    string(APPEND text
      "VS_VERSION_INFO VERSIONINFO\n"
      "FILEVERSION ${v}\nPRODUCTVERSION ${v}\n"
      "FILEOS VOS_NT_WINDOWS32\nFILETYPE VFT_APP\n"
      "BEGIN\n"
      "  BLOCK \"StringFileInfo\"\n  BEGIN\n    BLOCK \"040904B0\"\n    BEGIN\n"
      "      VALUE \"CompanyName\", \"funkpower\"\n"
      "      VALUE \"FileDescription\", \"${ARGV1}\"\n"
      "      VALUE \"FileVersion\", \"${PROJECT_VERSION}\"\n"
      "      VALUE \"InternalName\", \"${target}\"\n"
      "      VALUE \"OriginalFilename\", \"${target}.exe\"\n"
      "      VALUE \"ProductName\", \"Blockwright\"\n"
      "      VALUE \"ProductVersion\", \"${PROJECT_VERSION}\"\n"
      "    END\n  END\n"
      "  BLOCK \"VarFileInfo\"\n  BEGIN\n    VALUE \"Translation\", 0x409, 1200\n  END\n"
      "END\n")
  endif()
  file(CONFIGURE OUTPUT ${rc} CONTENT "${text}")
  target_sources(${target} PRIVATE ${rc})
  set_property(SOURCE ${rc} APPEND PROPERTY OBJECT_DEPENDS ${BW_WINDOWS_MANIFEST})
endfunction()

# A cross build's programs run under wine (when it is installed): the tests,
# and Catch's discovery of them. wine-run puts the Qt and llvm-mingw DLLs on
# the program's PATH, passes QT_QPA_PLATFORM and the other QT_* variables on
# (wine itself drops them) and keeps its own prefix in the build tree unless
# BW_WINEPREFIX names another.
if(CMAKE_CROSSCOMPILING AND NOT CMAKE_CROSSCOMPILING_EMULATOR)
  find_program(BW_WINE wine)
  if(BW_WINE)
    get_filename_component(bw_qt_bin "${Qt6_DIR}/../../../bin" ABSOLUTE)
    set(bw_winepath "")
    # llvm-mingw's runtime first: Qt's bin has an older libc++.dll that lacks
    # what newer compilers' code imports.
    foreach(dir ${LLVM_MINGW}/x86_64-w64-mingw32/bin ${bw_qt_bin})
      string(REPLACE "/" "\\\\" dir "Z:${dir}")
      list(APPEND bw_winepath "${dir}")
    endforeach()
    list(JOIN bw_winepath ";" bw_winepath)
    # The offscreen platform the tests run on finds no font under wine and
    # draws every character as a 12-pixel box, so docks and the window come
    # out at other sizes than anywhere else. It reads fonts from
    # QT_QPA_FONTDIR: a copy of one sans family from the host's fonts (the
    # one fontconfig gives Linux's run, where there is a choice) goes there.
    set(BW_WINE_FONTS "" CACHE STRING "The TrueType files the tests under wine draw with (empty: look in /usr/share/fonts)")
    set(bw_fonts "${BW_WINE_FONTS}")
    if(NOT bw_fonts)
      foreach(pattern noto/NotoSans-*.ttf TTF/NotoSans-*.ttf truetype/noto/NotoSans-*.ttf
                      dejavu/DejaVuSans*.ttf TTF/DejaVuSans*.ttf truetype/dejavu/DejaVuSans*.ttf)
        file(GLOB bw_fonts /usr/share/fonts/${pattern})
        if(bw_fonts)
          break()
        endif()
      endforeach()
    endif()
    set(bw_fontdir "")
    if(bw_fonts)
      file(REMOVE_RECURSE ${PROJECT_BINARY_DIR}/wine-fonts)
      file(COPY ${bw_fonts} DESTINATION ${PROJECT_BINARY_DIR}/wine-fonts)
      set(bw_fontdir "export QT_QPA_FONTDIR=\"\${QT_QPA_FONTDIR-Z:${PROJECT_BINARY_DIR}/wine-fonts}\"")
    else()
      message(WARNING "No sans fonts found for the tests under wine; set BW_WINE_FONTS. Layout tests will fail.")
    endif()
    file(CONFIGURE OUTPUT ${PROJECT_BINARY_DIR}/wine-run CONTENT [[#!/bin/sh
# Runs one of this build's Windows programs under wine.
export WINEPREFIX="${BW_WINEPREFIX:-@PROJECT_BINARY_DIR@/wineprefix}"
export WINEDEBUG="${WINEDEBUG:--all}"
export WINEDLLOVERRIDES="${WINEDLLOVERRIDES:-mscoree,mshtml=}"
export WINEPATH="@bw_winepath@"
# No Vulkan driver: some print a warning into the output a test reads.
export VK_LOADER_DRIVERS_DISABLE="${VK_LOADER_DRIVERS_DISABLE-*}"
@bw_fontdir@
# Wine hands a program QT_* variables only from WINEQT_* ones.
for v in $(env | sed -n 's/^\(QT_[A-Za-z0-9_]*\)=.*/\1/p'); do
  eval "export WINE$v=\"\$$v\""
done
# The first program of a wine session starts wine's services, which keep its
# output open while the session lasts: ctest would wait on them. Start them
# with none of the caller's descriptors when no session is running.
mkdir -p "$WINEPREFIX"
server="/tmp/.wine-$(id -u)/server-$(stat -Lc %D "$WINEPREFIX")-$(printf %x "$(stat -Lc %i "$WINEPREFIX")")"
[ -S "$server/socket" ] || "@BW_WINE@" wineboot </dev/null >/dev/null 2>&1
exec "@BW_WINE@" "$@"
]] @ONLY)
    file(CHMOD ${PROJECT_BINARY_DIR}/wine-run PERMISSIONS OWNER_READ OWNER_WRITE OWNER_EXECUTE GROUP_READ
         GROUP_EXECUTE WORLD_READ WORLD_EXECUTE)
    set(CMAKE_CROSSCOMPILING_EMULATOR ${PROJECT_BINARY_DIR}/wine-run)
  endif()
endif()
