cmake_minimum_required(VERSION 3.16)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)

# waveshare/esp32_p4_wifi6_touch_lcd_7b (as of v2.0.0) calls memcpy() in
# esp32_p4_wifi6_touch_lcd_7b.c without including <string.h>. It's a
# vendored/managed component re-fetched fresh on every build, so we can't
# just add the include there - relax this one warning project-wide
# instead of failing the whole build over a missing header upstream. Must
# be set before project() - that's what actually processes components.
idf_build_set_property(COMPILE_OPTIONS "-Wno-error=implicit-function-declaration" APPEND)

# Set explicitly rather than relying on `git describe` (which ESP-IDF
# otherwise prefers for a project inside a git repo) so the embedded app
# version doesn't drift between tagged releases.
set(PROJECT_VER "v0.2.4")

project(esp32_p4_vnc_viewer)
