// Provides the one libbase symbol the Android 14 fingerprint HAL needs that
// Android 13's libbase (VNDK v33) lacks:
//   bool android::base::WriteStringToFd(std::string_view, android::base::borrowed_fd)
// On x86_64 a libc++ std::string_view {data, size} is passed in two registers
// and borrowed_fd (a trivially copyable int) in a third, so a C function with
// (const char*, size_t, int) has the same ABI.
#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <unistd.h>

bool WriteStringToFd_sv(const char* data, size_t size, int fd) __asm__(
    "_ZN7android4base15WriteStringToFdENSt3__117basic_string_viewIcNS1_11char_traitsIcEEEENS0_11borrowed_fdE");

__attribute__((visibility("default")))
bool WriteStringToFd_sv(const char* data, size_t size, int fd) {
    while (size > 0) {
        ssize_t n = write(fd, data, size);
        if (n < 0) {
            if (errno == EINTR) continue;
            return false;
        }
        data += n;
        size -= (size_t)n;
    }
    return true;
}
