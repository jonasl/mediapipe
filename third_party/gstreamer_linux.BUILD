licenses(["notice"])  # LGPL license

# The following build rule assumes that GStreamer is installed by
# 'apt-get install libgstreamer1.0-dev libgstreamer-plugins-base1.0-dev \'
# '                gstreamer1.0-plugins-good' on Debian/Ubuntu.
# If you install GStreamer separately, please modify the build rule accordingly.

# TODO: There's gotta be a better way to include a system library than to
# decalre its entire depencency graph here? This is insane.

cc_library(
    name = "gstreamer",
    srcs = glob(
        [
            "lib/aarch64-linux-gnu/libgstbase-1.0.so",
            "lib/aarch64-linux-gnu/libgstreamer-1.0.so",
            "lib/aarch64-linux-gnu/libgstallocators-1.0.so",
            "lib/aarch64-linux-gnu/libgstapp-1.0.so",
            "lib/aarch64-linux-gnu/libgstvideo-1.0.so",
            "lib/aarch64-linux-gnu/libgstgl-1.0.so",
        ],
    ),
    hdrs = glob(
        [
            "include/gstreamer-1.0/gst/*.h",
            "include/gstreamer-1.0/gst/app/*.h",
            "include/gstreamer-1.0/gst/base/*.h",
            "include/gstreamer-1.0/gst/gl/*.h",
            "lib/aarch64-linux-gnu/gstreamer-1.0/include/gst/gl/*.h",
            "include/gstreamer-1.0/gst/video/*.h",
        ]),
    includes = ["include/gstreamer-1.0/", "lib/aarch64-linux-gnu/gstreamer-1.0/include"],
    linkstatic = 0,
    visibility = ["//visibility:public"],
    deps = [
        ":glib",
    ],
)

cc_library(
    name = "glib",
    srcs = glob(
        [
            "lib/aarch64-linux-gnu/libglib-2.0.so",
            "lib/aarch64-linux-gnu/libgobject-2.0.so",
        ],
    ),
    hdrs = glob(
        [
            "include/glib-2.0/*.h",
            "include/glib-2.0/*/*.h",
            "include/glib-2.0/glib/deprecated/*.h",
            "include/glib-2.0/gobject/*.h",
            "lib/aarch64-linux-gnu/glib-2.0/include/*",
        ]),
    includes = ["include/glib-2.0", "lib/aarch64-linux-gnu/glib-2.0/include"],
    linkstatic = 0,
    visibility = ["//visibility:public"],
)

# sudo apt-get install libgtk-3-dev
cc_library(
    name = "gtk",
    srcs = glob(
        [
            "lib/aarch64-linux-gnu/libgdk-3.so",
            "lib/aarch64-linux-gnu/libgtk-3.so",
        ],
    ),
    hdrs = glob(
        [
            "include/gtk-3.0/*.h",
            "include/gtk-3.0/*/*.h",
            "include/gtk-3.0/*/*/*.h",
            "include/gtk-3.0/*/deprecated/*.h",
        ]),
    includes = ["include/gtk-3.0"],
    linkstatic = 0,
    visibility = ["//visibility:public"],
    deps = [
        ":atk",
        ":gdk-pixbuf",
        ":cairo",
        ":glib",
        ":pango",
    ],
)

cc_library(
    name = "pango",
    srcs = glob(
        [
            "lib/aarch64-linux-gnu/libpango-1.0.so",
        ],
    ),
    hdrs = glob(
        [
            "include/pango-1.0/*/*.h",
        ]),
    includes = ["include/pango-1.0"],
    linkstatic = 0,
    visibility = ["//visibility:public"],
)

cc_library(
    name = "cairo",
    srcs = glob(
        [
            "lib/aarch64-linux-gnu/libcairo.so",
        ],
    ),
    hdrs = glob(
        [
            "include/cairo/*.h",
        ]),
    includes = ["include/cairo"],
    linkstatic = 0,
    visibility = ["//visibility:public"],
)

cc_library(
    name = "gdk-pixbuf",
    srcs = glob(
        [
        ],
    ),
    hdrs = glob(
        [
            "include/gdk-pixbuf-2.0/*.h",
            "include/gdk-pixbuf-2.0/*/*.h",
        ]),
    includes = ["include/gdk-pixbuf-2.0"],
    linkstatic = 0,
    visibility = ["//visibility:public"],
)

cc_library(
    name = "atk",
    srcs = glob(
        [
        ],
    ),
    hdrs = glob(
        [
            "include/atk-1.0/*.h",
            "include/atk-1.0/*/*.h",
        ]),
    includes = ["include/atk-1.0"],
    linkstatic = 0,
    visibility = ["//visibility:public"],
)

cc_library(
    name = "gbm",
    srcs = glob(
        [
            "lib/aarch64-linux-gnu/libgbm.so",
        ],
    ),
    hdrs = glob(
        [
            "include/gbm.h",
        ]),
    includes = ["include"],
    linkstatic = 0,
    visibility = ["//visibility:public"],
)

cc_library(
    name = "drm",
    srcs = glob(
        [
            "lib/aarch64-linux-gnu/libdrm.so",
        ],
    ),
    hdrs = glob(
        [
            "include/xf86drm.h",
            "include/libdrm/*.h"
        ]),
    includes = ["include", "include/libdrm"],
    linkstatic = 0,
    visibility = ["//visibility:public"],
)
