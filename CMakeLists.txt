cmake_minimum_required(VERSION 3.16)

# Set explicitly rather than relying on `git describe` (which ESP-IDF
# otherwise prefers for a project inside a git repo) so the embedded app
# version doesn't drift between tagged releases.
set(PROJECT_VER "v0.1.1")

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(esp32_p4_vnc_viewer)
