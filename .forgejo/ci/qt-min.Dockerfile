# The minimum-Qt CI job's image (.forgejo/workflows/ci.yml, job
# "linux-qt-min"): the main image's Arch base and tools, with Qt's own build of
# the oldest Qt minor version that CMakeLists.txt accepts (find_package(Qt6
# 6.7)) in /opt/qt instead of Arch's Qt. Built on the runner's host by
# .forgejo/ci/build-image; the runner never pulls or builds it, so a job
# downloads no Qt.
#
# QT_VERSION is the last patch release of the minor version CMakeLists.txt's
# find_package(Qt6) asks for, because 6.7.0 to 6.7.2 don't build with a
# current GCC: their qcomparehelpers.h declares IsFloatType_v<_Float16>
# without inline, and every object file that includes it defines it again.
# Patch releases add no API. Bump this image's rev in build-image and ci.yml
# when QT_VERSION changes.
ARG QT_VERSION=6.7.3

FROM archlinux:base-devel@sha256:624bdc7fc6615155d67bc34cf7146c0c9fd0e3c92943977da63a630e4ca49c30 AS qt
ARG QT_VERSION
RUN pacman -Syu --noconfirm --needed python && pacman -Scc --noconfirm
RUN python -m venv /opt/aqt \
 && /opt/aqt/bin/pip install --no-cache-dir aqtinstall==3.3.0 \
 && /opt/aqt/bin/aqt install-qt -O /opt/qt linux desktop "$QT_VERSION" linux_gcc_64 \
      -m qtimageformats qtpdf \
 && rm -rf /opt/qt/"$QT_VERSION"/gcc_64/doc /opt/qt/"$QT_VERSION"/gcc_64/qml

FROM archlinux:base-devel@sha256:624bdc7fc6615155d67bc34cf7146c0c9fd0e3c92943977da63a630e4ca49c30
ARG QT_VERSION

# The same packages as arch.Dockerfile, but Arch's Qt is installed only for its
# dependencies (the libraries Qt's own build loads) and then removed, so CMake
# and the tests can find no Qt but /opt/qt's. qt6-webengine's place is taken by
# the qtpdf module, which brings the PDF image plugin the reference-layer tests
# refuse.
RUN pacman -Syu --noconfirm --needed \
      cmake ninja git qt6-base qt6-svg libzip zstd nss \
      xorg-server-xvfb xorg-xauth libxcb noto-fonts \
      python jq curl zip unzip file perl desktop-file-utils util-linux e2fsprogs \
 && pacman -Rdd --noconfirm qt6-base qt6-svg qt6-translations \
 && pacman -Scc --noconfirm

# The loader finds /opt/qt's libraries as it would a system Qt's: an installed
# Blockwright has no RPATH, and the install tests run it.
COPY --from=qt /opt/qt /opt/qt
RUN echo "/opt/qt/$QT_VERSION/gcc_64/lib" > /etc/ld.so.conf.d/qt.conf && ldconfig
ENV QT_VERSION=$QT_VERSION \
    CMAKE_PREFIX_PATH=/opt/qt/$QT_VERSION/gcc_64 \
    PATH=/opt/qt/$QT_VERSION/gcc_64/bin:$PATH

# The jobs run as this user, not root: tests check that unreadable files and
# folders are refused, and root reads them anyway.
RUN useradd --create-home --uid 2000 --shell /bin/bash ci
USER ci
WORKDIR /home/ci
