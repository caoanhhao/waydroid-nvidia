/* List what the compositor advertises over zwp_linux_dmabuf_v1 at version 3
 * (format + modifier events), exactly what Waydroid's hwcomposer binds and
 * copies into waydroid.modifiers.<fourcc>.<n>.
 * usage: wl-dmabuf-mods [fourcc-hex, default 34324241 = AB24] */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wayland-client.h>
#include "linux-dmabuf-v1-client.h"

static uint32_t want = 0x34324241;
static int count;

static void
format(void *data, struct zwp_linux_dmabuf_v1 *d, uint32_t fmt)
{
}

static void
modifier(void *data, struct zwp_linux_dmabuf_v1 *d, uint32_t fmt, uint32_t hi,
         uint32_t lo)
{
   if (fmt != want)
      return;
   printf("  0x%016llx\n", (unsigned long long)(((uint64_t)hi << 32) | lo));
   count++;
}

static const struct zwp_linux_dmabuf_v1_listener dmabuf_listener = { format, modifier };

static void
global(void *data, struct wl_registry *r, uint32_t name, const char *iface,
       uint32_t version)
{
   if (!strcmp(iface, zwp_linux_dmabuf_v1_interface.name)) {
      printf("zwp_linux_dmabuf_v1 version %u (binding 3)\n", version);
      struct zwp_linux_dmabuf_v1 *d =
         wl_registry_bind(r, name, &zwp_linux_dmabuf_v1_interface, 3);
      zwp_linux_dmabuf_v1_add_listener(d, &dmabuf_listener, NULL);
   }
}

static void
global_remove(void *data, struct wl_registry *r, uint32_t name)
{
}

static const struct wl_registry_listener registry_listener = { global, global_remove };

int
main(int argc, char **argv)
{
   if (argc > 1)
      want = strtoul(argv[1], NULL, 16);
   struct wl_display *dpy = wl_display_connect(NULL);
   if (!dpy) {
      fprintf(stderr, "no wayland display\n");
      return 1;
   }
   struct wl_registry *reg = wl_display_get_registry(dpy);
   wl_registry_add_listener(reg, &registry_listener, NULL);
   wl_display_roundtrip(dpy);
   printf("modifiers for fourcc 0x%08x:\n", want);
   wl_display_roundtrip(dpy);
   wl_display_roundtrip(dpy);
   printf("%d modifiers\n", count);
   return 0;
}
