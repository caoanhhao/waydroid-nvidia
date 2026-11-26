/*
 * Allocate a YV12 AHardwareBuffer, lock it for CPU writes through gralloc4 and
 * check that the chroma planes are where Android YV12 (and Mesa) put them.
 * Exit 0 = PASS.  See venus/notes-camera.md.
 */
#include <android/hardware_buffer.h>
#include <stdio.h>

#define FORMAT_YV12 0x32315659 /* HAL_PIXEL_FORMAT_YV12 */
#define ALIGN(v, a) (((v) + (a) - 1) / (a) * (a))

int
main(void)
{
   AHardwareBuffer_Desc desc = {
      .width = 1920, .height = 1080, .layers = 1, .format = FORMAT_YV12,
      .usage = AHARDWAREBUFFER_USAGE_CPU_WRITE_OFTEN | AHARDWAREBUFFER_USAGE_GPU_SAMPLED_IMAGE,
   };
   AHardwareBuffer *buf;
   if (AHardwareBuffer_allocate(&desc, &buf) != 0) {
      printf("FAIL: allocate\n");
      return 1;
   }

   AHardwareBuffer_Planes p;
   if (AHardwareBuffer_lockPlanes(buf, AHARDWAREBUFFER_USAGE_CPU_WRITE_OFTEN, -1, NULL, &p) != 0 ||
       p.planeCount != 3) {
      printf("FAIL: lockPlanes\n");
      return 1;
   }
   char *base = p.planes[0].data;
   for (unsigned i = 0; i < 3; i++)
      printf("plane%u (%c) offset %td pixelStride %u rowStride %u\n", i, "YUV"[i],
             (char *)p.planes[i].data - base, p.planes[i].pixelStride, p.planes[i].rowStride);

   /* lockPlanes order is Y, U (Cb), V (Cr); YV12 stores V before U */
   unsigned ys = p.planes[0].rowStride, cs = ALIGN(ys / 2, 16), h = desc.height;
   long v_off = (long)ys * h, u_off = v_off + (long)cs * (h / 2);
   int ok = (char *)p.planes[2].data - base == v_off && (char *)p.planes[1].data - base == u_off &&
            p.planes[1].rowStride == cs && p.planes[2].rowStride == cs &&
            p.planes[1].pixelStride == 1 && p.planes[2].pixelStride == 1;
   printf("expected V at %ld, U at %ld, chroma rowStride %u: %s\n", v_off, u_off, cs,
          ok ? "PASS" : "FAIL");

   AHardwareBuffer_unlock(buf, NULL);
   AHardwareBuffer_release(buf);
   return ok ? 0 : 1;
}
