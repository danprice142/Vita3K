// Vita3K emulator project - libretro core
// Provides stb_image implementation that is normally compiled in gui/src/gui.cpp
// which is excluded from libretro builds.

#define STB_IMAGE_IMPLEMENTATION
#include <stb_image.h>
