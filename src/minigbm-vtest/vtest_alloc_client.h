/*
 * Copyright 2026 waydroid-nvidia contributors
 * SPDX-License-Identifier: MIT
 *
 * Minimal vtest client for VCMD_ALLOC_DMABUF_IMAGE (vtest protocol >= 6):
 * asks the host virgl_test_server to allocate a window buffer on the host GPU
 * and returns it as a dma-buf.  No venus context is created.
 */

#ifndef VTEST_ALLOC_CLIENT_H
#define VTEST_ALLOC_CLIENT_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct vtest_alloc_buffer {
   int fd;
   uint64_t modifier;
   uint32_t stride;
   uint32_t offset;
   uint64_t size;
};

/* Connects and negotiates the protocol.  Returns a socket fd, or a negative
 * errno (-EPROTONOSUPPORT when the server is older than protocol 6).
 */
int
vtest_alloc_connect(const char *socket_path, const char *client_name);

/* flags: enum vcmd_alloc_dmabuf_image_flag.  modifiers may be NULL when
 * modifier_count is 0 (any modifier the host supports).
 * Returns 0, a negative errno from the server, or -EPIPE when the connection
 * is broken (the caller should close and reconnect).
 */
int
vtest_alloc_dmabuf_image(int sock,
                         uint32_t width,
                         uint32_t height,
                         uint32_t drm_format,
                         uint32_t flags,
                         const uint64_t *modifiers,
                         uint32_t modifier_count,
                         struct vtest_alloc_buffer *out);

#ifdef __cplusplus
}
#endif

#endif /* VTEST_ALLOC_CLIENT_H */
