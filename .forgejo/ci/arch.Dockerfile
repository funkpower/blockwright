# The main CI job's image (.forgejo/workflows/ci.yml, job "linux"): Arch Linux,
# so a current Qt 6, on a base pinned by digest. Built on the runner's host by
# .forgejo/ci/build-image; the runner never pulls or builds it.
FROM archlinux:base-devel@sha256:624bdc7fc6615155d67bc34cf7146c0c9fd0e3c92943977da63a630e4ca49c30

# Catch2 is left out on purpose: tests/CMakeLists.txt then fetches the version
# it pins. Noto Sans is the sans-serif the dock layout tests are measured in.
# qt6-webengine is there for Qt's PDF image plugin (Arch ships it in that
# package): the reference-layer tests check that a PDF is refused.
RUN pacman -Syu --noconfirm --needed \
      cmake ninja git qt6-base qt6-svg qt6-webengine libzip zstd \
      xorg-server-xvfb xorg-xauth libxcb noto-fonts \
      python jq curl zip unzip file perl desktop-file-utils util-linux e2fsprogs \
 && pacman -Scc --noconfirm

# The jobs run as this user, not root: tests check that unreadable files and
# folders are refused, and root reads them anyway.
RUN useradd --create-home --uid 2000 --shell /bin/bash ci
USER ci
WORKDIR /home/ci
