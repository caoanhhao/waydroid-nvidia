/* Stand-in for the original libgbm_mesa_wrapper.so in host tests: records
 * that the shim fell back to it and returns a recognizable result.
 */
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#include "gbm_mesa_wrapper.h"

int fake_inner_alloc_calls;

static uint32_t
get_format(uint32_t drm_format)
{
   return drm_format;
}

static struct gbm_device *
dev_create(int fd)
{
   (void)fd;
   return (struct gbm_device *)0x1;
}

static void
dev_destroy(struct gbm_device *gbm)
{
   (void)gbm;
}

static int
alloc(struct alloc_args *args)
{
   fake_inner_alloc_calls++;
   args->out_fd = -1;
   args->out_stride = 0xdead;
   args->out_modifier = 0;
   return 0;
}

static struct gbm_ops ops = {
   .get_gbm_format = get_format,
   .dev_create = dev_create,
   .dev_destroy = dev_destroy,
   .alloc = alloc,
};

__attribute__((visibility("default"))) struct gbm_ops *
get_gbm_ops(void)
{
   return &ops;
}
