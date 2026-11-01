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

On the dev machine, with Catch2 3 installed (or its prefix in
`CMAKE_PREFIX_PATH`):

```sh
scripts/release --check ~/.cache/dw/bw-release-check <version>
```

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

## 4. macOS (dmg and Homebrew cask)

On a Mac at the tag: build the dmg, upload it with its `.sha256`, and publish
the cask, as in [README.md § Homebrew](README.md#homebrew).

## 5. Windows (MSI)

On the dev machine at the tag: `scripts/package-windows`, as in
[windows/README.md § The MSI](windows/README.md#the-msi), then upload
`dist/blockwright-<version>-x86_64.msi`.

## 6. After publishing

- Install each package from where users get it and run `blockwright --version`
  and `blockwright-cli --version`: `yay -S blockwright` on Arch, the dmg and
  `brew install --cask blockwright` on a Mac, the MSI on Windows.
- Note the release in the issue tracker if an issue was waiting on it.
