#ifndef SPLASH_ANIM_H
#define SPLASH_ANIM_H

#include <stdint.h>

// Render one frame of the boot splash into an XRGB8888 buffer.
//   stride   row pitch in pixels (not bytes)
//   t        seconds since the splash started
//   progress boot progress 0..1 (drives the bar)
void splash_render(uint32_t *buf, int w, int h, int stride, float t, float progress);

#endif
