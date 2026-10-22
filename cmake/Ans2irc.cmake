# ans2irc, the IRC encoder (PLAN §8): a Rust crate in the third_party/ans2irc
# submodule, linked as a static library through the C ABI in ffi/ans2irc-ffi.
# Corrosion drives cargo, so the build needs a Rust toolchain.

if(NOT EXISTS ${PROJECT_SOURCE_DIR}/third_party/ans2irc/Cargo.toml)
  message(FATAL_ERROR "third_party/ans2irc is empty: run `git submodule update --init` "
                      "(or clone with --recursive)")
endif()

include(FetchContent)
FetchContent_Declare(Corrosion
  URL https://github.com/corrosion-rs/corrosion/archive/refs/tags/v0.6.1.tar.gz
  URL_HASH SHA256=e9e95b1ee2bad52681f347993fb1a5af5cce458c5ce8a2636c9e476e4babf8e3
  DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
FetchContent_MakeAvailable(Corrosion)

# Target ans2irc_ffi; its header is ffi/ans2irc-ffi/include/ans2irc.h.
corrosion_import_crate(MANIFEST_PATH ${PROJECT_SOURCE_DIR}/ffi/ans2irc-ffi/Cargo.toml
                       CRATE_TYPES staticlib
                       LOCKED)
target_include_directories(ans2irc_ffi INTERFACE ${PROJECT_SOURCE_DIR}/ffi/ans2irc-ffi/include)

# The ans2irc CLI from the same commit, as the tests' reference.
if(BUILD_TESTING)
  corrosion_import_crate(MANIFEST_PATH ${PROJECT_SOURCE_DIR}/third_party/ans2irc/Cargo.toml
                         CRATES ans2irc
                         LOCKED)
endif()
