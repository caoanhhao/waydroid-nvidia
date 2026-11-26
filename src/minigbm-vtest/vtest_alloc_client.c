/*
 * Copyright 2026 waydroid-nvidia contributors
 * SPDX-License-Identifier: MIT
 */

#include "vtest_alloc_client.h"

#include <errno.h>
#include <stdbool.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include "vtest_protocol.h"

#define ALLOC_PROTOCOL_VERSION 6

static int
write_all(int sock, const void *buf, size_t size)
{
   const char *p = buf;
   while (size) {
      const ssize_t ret = write(sock, p, size);
      if (ret < 0 && errno == EINTR)
         continue;
      if (ret <= 0)
         return -EPIPE;
      p += ret;
      size -= ret;
   }
   return 0;
}

static int
read_all(int sock, void *buf, size_t size)
{
   char *p = buf;
   while (size) {
      const ssize_t ret = read(sock, p, size);
      if (ret < 0 && errno == EINTR)
         continue;
      if (ret <= 0)
         return -EPIPE;
      p += ret;
      size -= ret;
   }
   return 0;
}

static int
receive_fd(int sock)
{
   char cmsg_buf[CMSG_SPACE(sizeof(int))];
   char dummy;
   struct iovec iov = { .iov_base = &dummy, .iov_len = sizeof(dummy) };
   struct msghdr msg = {
      .msg_iov = &iov,
      .msg_iovlen = 1,
      .msg_control = cmsg_buf,
      .msg_controllen = sizeof(cmsg_buf),
   };

   ssize_t ret;
   do {
      ret = recvmsg(sock, &msg, MSG_CMSG_CLOEXEC);
   } while (ret < 0 && errno == EINTR);
   if (ret <= 0)
      return -EPIPE;

   struct cmsghdr *cmsg = CMSG_FIRSTHDR(&msg);
   if (!cmsg || cmsg->cmsg_level != SOL_SOCKET || cmsg->cmsg_type != SCM_RIGHTS)
      return -EPIPE;

   int fd;
   memcpy(&fd, CMSG_DATA(cmsg), sizeof(fd));
   return fd;
}

static int
negotiate_version(int sock, uint32_t *out_version)
{
   uint32_t hdr[VTEST_HDR_SIZE];
   uint32_t busy_wait[VCMD_BUSY_WAIT_SIZE] = { 0, 0 };
   uint32_t dummy;
   int ret;

   /* ping, plus a dummy busy wait so an old server without ping still
    * answers something (same as Mesa's vtest backend)
    */
   hdr[VTEST_CMD_LEN] = VCMD_PING_PROTOCOL_VERSION_SIZE;
   hdr[VTEST_CMD_ID] = VCMD_PING_PROTOCOL_VERSION;
   if ((ret = write_all(sock, hdr, sizeof(hdr))))
      return ret;
   hdr[VTEST_CMD_LEN] = VCMD_BUSY_WAIT_SIZE;
   hdr[VTEST_CMD_ID] = VCMD_RESOURCE_BUSY_WAIT;
   if ((ret = write_all(sock, hdr, sizeof(hdr))) ||
       (ret = write_all(sock, busy_wait, sizeof(busy_wait))))
      return ret;

   if ((ret = read_all(sock, hdr, sizeof(hdr))))
      return ret;
   const bool has_ping = hdr[VTEST_CMD_ID] == VCMD_PING_PROTOCOL_VERSION;
   if (has_ping && (ret = read_all(sock, hdr, sizeof(hdr))))
      return ret;
   if ((ret = read_all(sock, &dummy, sizeof(dummy))))
      return ret;
   if (!has_ping)
      return -EPROTONOSUPPORT;

   uint32_t version = ALLOC_PROTOCOL_VERSION;
   hdr[VTEST_CMD_LEN] = VCMD_PROTOCOL_VERSION_SIZE;
   hdr[VTEST_CMD_ID] = VCMD_PROTOCOL_VERSION;
   if ((ret = write_all(sock, hdr, sizeof(hdr))) ||
       (ret = write_all(sock, &version, sizeof(version))) ||
       (ret = read_all(sock, hdr, sizeof(hdr))) ||
       (ret = read_all(sock, &version, sizeof(version))))
      return ret;

   *out_version = version;
   return 0;
}

int
vtest_alloc_connect(const char *socket_path, const char *client_name)
{
   struct sockaddr_un un = { .sun_family = AF_UNIX };
   if (strlen(socket_path) >= sizeof(un.sun_path))
      return -ENAMETOOLONG;
   strcpy(un.sun_path, socket_path);

   const int sock = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
   if (sock < 0)
      return -errno;
   if (connect(sock, (struct sockaddr *)&un, sizeof(un)) < 0) {
      const int err = -errno;
      close(sock);
      return err;
   }

   /* the first command must be VCMD_CREATE_RENDERER */
   const uint32_t name_size = strlen(client_name) + 1;
   uint32_t hdr[VTEST_HDR_SIZE] = { name_size, VCMD_CREATE_RENDERER };
   uint32_t version = 0;
   int ret = write_all(sock, hdr, sizeof(hdr));
   if (!ret)
      ret = write_all(sock, client_name, name_size);
   if (!ret)
      ret = negotiate_version(sock, &version);
   if (!ret && version < ALLOC_PROTOCOL_VERSION)
      ret = -EPROTONOSUPPORT;
   if (ret) {
      close(sock);
      return ret;
   }

   return sock;
}

int
vtest_alloc_dmabuf_image(int sock,
                         uint32_t width,
                         uint32_t height,
                         uint32_t drm_format,
                         uint32_t flags,
                         const uint64_t *modifiers,
                         uint32_t modifier_count,
                         struct vtest_alloc_buffer *out)
{
   uint32_t req[VCMD_ALLOC_DMABUF_IMAGE_SIZE(VCMD_ALLOC_DMABUF_IMAGE_MAX_MODIFIERS)];
   uint32_t hdr[VTEST_HDR_SIZE];
   uint32_t resp[VCMD_ALLOC_DMABUF_IMAGE_RESP_SIZE];
   int ret;

   if (modifier_count > VCMD_ALLOC_DMABUF_IMAGE_MAX_MODIFIERS)
      modifier_count = VCMD_ALLOC_DMABUF_IMAGE_MAX_MODIFIERS;

   const uint32_t len = VCMD_ALLOC_DMABUF_IMAGE_SIZE(modifier_count);
   req[VCMD_ALLOC_DMABUF_IMAGE_WIDTH] = width;
   req[VCMD_ALLOC_DMABUF_IMAGE_HEIGHT] = height;
   req[VCMD_ALLOC_DMABUF_IMAGE_FORMAT] = drm_format;
   req[VCMD_ALLOC_DMABUF_IMAGE_FLAGS] = flags;
   req[VCMD_ALLOC_DMABUF_IMAGE_MODIFIER_COUNT] = modifier_count;
   for (uint32_t i = 0; i < modifier_count; i++) {
      req[VCMD_ALLOC_DMABUF_IMAGE_MODIFIER_LO(i)] = (uint32_t)modifiers[i];
      req[VCMD_ALLOC_DMABUF_IMAGE_MODIFIER_HI(i)] = (uint32_t)(modifiers[i] >> 32);
   }

   hdr[VTEST_CMD_LEN] = len;
   hdr[VTEST_CMD_ID] = VCMD_ALLOC_DMABUF_IMAGE;
   if ((ret = write_all(sock, hdr, sizeof(hdr))) ||
       (ret = write_all(sock, req, len * sizeof(uint32_t))) ||
       (ret = read_all(sock, hdr, sizeof(hdr))))
      return ret;
   if (hdr[VTEST_CMD_ID] != VCMD_ALLOC_DMABUF_IMAGE ||
       hdr[VTEST_CMD_LEN] != VCMD_ALLOC_DMABUF_IMAGE_RESP_SIZE)
      return -EPIPE;
   if ((ret = read_all(sock, resp, sizeof(resp))))
      return ret;

   if (resp[VCMD_ALLOC_DMABUF_IMAGE_RESP_ERROR])
      return -(int)resp[VCMD_ALLOC_DMABUF_IMAGE_RESP_ERROR];

   const int fd = receive_fd(sock);
   if (fd < 0)
      return fd;

   out->fd = fd;
   out->modifier = resp[VCMD_ALLOC_DMABUF_IMAGE_RESP_MODIFIER_LO] |
                   (uint64_t)resp[VCMD_ALLOC_DMABUF_IMAGE_RESP_MODIFIER_HI] << 32;
   out->stride = resp[VCMD_ALLOC_DMABUF_IMAGE_RESP_STRIDE];
   out->offset = resp[VCMD_ALLOC_DMABUF_IMAGE_RESP_OFFSET];
   out->size = resp[VCMD_ALLOC_DMABUF_IMAGE_RESP_SIZE_LO] |
               (uint64_t)resp[VCMD_ALLOC_DMABUF_IMAGE_RESP_SIZE_HI] << 32;
   return 0;
}
