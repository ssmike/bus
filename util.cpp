#include "util.h"

namespace bus::internal {

void write_header(size_t size, char* buf) {
    for (size_t i = 0; i < header_len; ++i) {
        buf[i] = size & 255;
        size /= 256;
    }
}

size_t read_header(char* buf) {
    size_t result = 0;
    for (ssize_t i = header_len - 1; i >= 0; --i) {
        result = result * 256 + static_cast<unsigned char>(buf[i]);
    }
    return result;
}

}
