#pragma once
#include <GfxRenderer.h>

#include <cstddef>

#include "ScreenshotInfo.h"

class ScreenshotUtil {
 public:
  static void takeScreenshot(GfxRenderer& renderer);
  static bool saveFramebufferAsBmp(const char* filename, const uint8_t* framebuffer, int width, int height);

  // Orientation-aware variant. Writes a BMP whose dimensions match the logical
  // screen for `orientation` (480x800 portrait, 800x480 landscape) and inverts
  // the rotateCoordinates() mapping to read framebuffer pixels. Use this when
  // the framebuffer was rendered in an orientation other than Portrait, since
  // saveFramebufferAsBmp always assumes Portrait when rotating.
  static bool saveFramebufferAsBmpOriented(const char* filename, const uint8_t* framebuffer, int panelWidth,
                                           int panelHeight, GfxRenderer::Orientation orientation);

 private:
  static void buildFilename(const ScreenshotInfo& info, char* buf, size_t bufSize);
};
