// LD_PRELOAD shim for cli_rlimit_test.sh: a setrlimit of RLIMIT_NOFILE fails
// with EPERM, as macOS fails a raise past OPEN_MAX. Every other resource goes
// to the real setrlimit, so a sanitizer runtime's RLIMIT_CORE call at startup
// still works. The definitions take their symbol names from asm labels, not
// from <sys/resource.h>'s declarations: under _FILE_OFFSET_BITS=64 glibc
// redirects setrlimit to setrlimit64, and two definitions would then share
// one symbol. The resource is an int and the limit opaque, which matches the
// ABI of glibc (whose __rlimit_resource_t is an int-sized enum), FreeBSD and
// musl alike.
#include <cerrno>
#include <dlfcn.h>
#include <sys/resource.h>

namespace {
int passOrFail(const char* name, int resource, const void* limit) {
    if (resource == RLIMIT_NOFILE) {
        errno = EPERM;
        return -1;
    }
    using Real = int (*)(int, const void*);
    auto real = reinterpret_cast<Real>(dlsym(RTLD_NEXT, name));
    if (!real) {
        errno = ENOSYS;
        return -1;
    }
    return real(resource, limit);
}
} // namespace

extern "C" int bwShimSetrlimit(int resource, const void* limit) __asm__("setrlimit");
extern "C" int bwShimSetrlimit(int resource, const void* limit) {
    return passOrFail("setrlimit", resource, limit);
}

#if defined(__GLIBC__)
// glibc's name for it under _FILE_OFFSET_BITS=64
extern "C" int bwShimSetrlimit64(int resource, const void* limit) __asm__("setrlimit64");
extern "C" int bwShimSetrlimit64(int resource, const void* limit) {
    return passOrFail("setrlimit64", resource, limit);
}
#endif
