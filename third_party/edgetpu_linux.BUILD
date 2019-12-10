licenses(["notice"])

# sudo apt-get install libedgetpu-dev
cc_library(
    name = "edgetpu",
    srcs = glob(
        [
            "lib/aarch64-linux-gnu/libedgetpu.so",
        ],
    ),
    hdrs = glob(
        [
            "include/edgetpu.h",
        ]),
    includes = ["include"],
    linkstatic = 0,
    visibility = ["//visibility:public"],
)

