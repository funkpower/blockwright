# Releasing

The checklist for a version, on every platform. How each package is built is
in [README.md](README.md) (macOS, Linux) and
[windows/README.md](windows/README.md) (Windows); this file is the order to do
it in.

Everything here is published as funkpower (PLAN §14). The repository and its
releases must be readable without signing in: the AUR, Homebrew and every
distribution builder download the release assets anonymously.

## 1. The version

1. On a branch, set `project(blockwright VERSION <version>)` in
   `CMakeLists.txt` and add the version to `<releases>` in
   `packaging/linux/online.pumpin.funkpower.blockwright.metainfo.xml`
   (`<release version="<version>" date="YYYY-MM-DD"/>`, newest first).
   In the same PR, add a `packaging/debian/changelog` entry for
   `<version>-1` (`dch -v <version>-1`) and set the `Version:` of
   `packaging/rpm/blockwright.spec` with a `%changelog` entry for
   `<version>-1`, and the version in the `.TH` line of
   `packaging/linux/blockwright.1` and `blockwright-cli.1`; the
   `linux_packages` test fails until they all match.
   Merge it through a PR like any other change.
2. Check `main` at that commit: the full `ctest` passes
   (`QT_QPA_PLATFORM=offscreen`) and the dev install says the new version
   (`blockwright-dev --version`).
3. Tag it and push the tag:

   ```sh
   git tag -a v<version> -m "Blockwright <version>" <commit>
   GIT_ASKPASS=$PWD/scripts/git-askpass git push origin v<version>
   ```

4. Create a release for the tag on the forge. Every asset below is uploaded to
   it; their URLs are
   `https://git.pumpin.online/funkpower/blockwright/releases/download/v<version>/<file>`.

## 2. The source tarball

First pin the docs: before tagging (in the version PR of step 1), set
`packaging/docs-commit` to the full hash of the blockwright-docs commit on
`main` whose pages the release ships. Then, at the tag, build the release's
programs and the help bundle from that commit, as in
[README.md § The help bundle](README.md#the-help-bundle):

```sh
cmake -S . -B ~/.cache/dw/bw-release-build -G Ninja -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=OFF
cmake --build ~/.cache/dw/bw-release-build
scripts/help-bundle --cli ~/.cache/dw/bw-release-build/cli/blockwright-cli \
  --app ~/.cache/dw/bw-release-build/app/blockwright ~/.cache/dw/bw-help-<version>
```

On the dev machine, with Catch2 3 installed (or its prefix in
`CMAKE_PREFIX_PATH`):

```sh
scripts/release --help-bundle ~/.cache/dw/bw-help-<version> --check ~/.cache/dw/bw-release-check <version>
```

The tarball carries the bundle as `help/`; the check's build installs it and
its `help_bundle_check` test checks it.

It packages the tag `v<version>` (`git archive`, tracked files only), then
unpacks the tarball, builds it with `BW_FETCH_CATCH2=OFF` and runs its tests
without network, and checks that `blockwright-cli --version` names the tag's
commit. It writes into `dist/`:

- `blockwright-<version>.tar.gz` and `blockwright-<version>.tar.gz.sha256`:
  upload both to the release;
- `aur/blockwright/PKGBUILD` and `.SRCINFO`: the AUR package of exactly that
  tarball (section 3). makepkg won't run as root, so a release made as root
  writes the PKGBUILD alone and says so; make its `.SRCINFO` as a normal user.

The same commit always gives the same tarball, so running it again, or on
another machine, gives the same checksum. `--help` lists the options.

## 3. Arch Linux (AUR)

`packaging/aur/blockwright/` is the template of the release package (its
`pkgver` and checksum are filled in by `scripts/release`);
`packaging/aur/blockwright-git/` builds the newest commit of `main` and
changes only when its build does.

1. Build the release package from the uploaded tarball, in a clean chroot if
   `devtools` is installed (`extra-x86_64-build`), else with `makepkg`:

   ```sh
   cd dist/aur/blockwright
   makepkg -sf          # downloads the tarball from the release, checks its sum, runs check()
   namcap PKGBUILD *.pkg.tar.zst
   ```

2. Copy `PKGBUILD` and `.SRCINFO` into a clone of the AUR repository
   (`ssh://aur@aur.archlinux.org/blockwright.git`), commit them as funkpower
   and push.
3. When `packaging/aur/blockwright-git/` has changed since it was last
   published, regenerate its `.SRCINFO` (`makepkg --printsrcinfo > .SRCINFO`),
   check it with `makepkg -sf` and `namcap`, and push both files to
   `ssh://aur@aur.archlinux.org/blockwright-git.git`.

## 4. Debian, Ubuntu and Fedora

No repository carries these yet: build each from the uploaded tarball, as in
[README.md § Debian and Ubuntu](README.md#debian-and-ubuntu) and
[§ Fedora](README.md#fedora), in a clean container or chroot of each release,
with `lintian` and `rpmlint` clean. The `.deb` of one release is for that
release only, so name the distribution in each file uploaded to the release
(`blockwright_<version>-1_ubuntu26.04_amd64.deb`, `blockwright-<version>-1.fc45.x86_64.rpm`).

## 5. AppImage

From the uploaded tarball, in a Debian 12 container with Qt 6.8.3:
`scripts/package-appimage`, as in
[README.md § AppImage](README.md#appimage); it checks the AppImage before it
writes the `.sha256`. Upload `blockwright-<version>-x86_64.AppImage` and its
`.sha256`. The tarball's `help/` is installed into it.

The Flatpak bundle, at the tag, with `flatpak` and `flatpak-builder`:
`FLATPAK_USER_DIR=~/.cache/dw/flatpak scripts/package-flatpak --ref v<version> --help-bundle ~/.cache/dw/bw-help-<version> --check`,
as in [README.md § Flatpak](README.md#flatpak). Upload
`blockwright-<version>-x86_64.flatpak` and its `.sha256`. Nothing goes to
Flathub.

## 6. macOS (dmg and Homebrew cask)

On a Mac at the tag, with Qt 6.8.3 (an LTS release that runs on macOS 12, the
oldest the app supports; Qt 6.10 and later need macOS 13): build the dmg,
upload it with its `.sha256`, and publish the cask, as in
[README.md § Homebrew](README.md#homebrew), passing `--help-bundle` the
bundle from section 2 (copied to the Mac) to `scripts/package-macos`.
`scripts/package-macos` refuses a
Qt built for a newer macOS than the deployment target before it builds
anything.

## 7. Windows (MSI)

On the dev machine at the tag: `scripts/package-windows --help-bundle ~/.cache/dw/bw-help-<version>`, as in
[windows/README.md § The MSI](windows/README.md#the-msi), then upload
`dist/blockwright-<version>-x86_64.msi`.

## 8. After publishing

- Install each package from where users get it and run `blockwright --version`
  and `blockwright-cli --version`, and open Help > Manual, which opens in the
  help window, not the browser: `yay -S blockwright` on Arch, the dmg and
  `brew install --cask funkpower/tap/blockwright` on a Mac, the MSI on
  Windows, the AppImage (`--version` and `--cli --version`) on a current Linux.
- Note the release in the issue tracker if an issue was waiting on it.
