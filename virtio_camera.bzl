load("//build/bazel_common_rules/dist:dist.bzl", "copy_to_dist_dir")
load("//build/kernel/kleaf:kernel.bzl", "ddk_module")

def _srcs():
    return [
        "virtio_camera_driver.c",
        "virtio_camera_vq.c",
        "virtio_camera_v4l2.c",
        "virtio_camera.h",
        "include/uapi/linux/virtio_camera.h",
    ]

def define_target_variant_module(target, variant):
    kernel_build_variant = "{}_{}".format(target, variant)
    rule_name = "{}_virtio_camera".format(kernel_build_variant)
    kernel_build = select({
        "//build/kernel/kleaf:socrepo_true": "//soc-repo:{}_base_kernel".format(kernel_build_variant),
        "//build/kernel/kleaf:socrepo_false": "//msm-kernel:{}".format(kernel_build_variant),
    })

    ddk_module(
        name = rule_name,
        out = "virtio_camera.ko",
        srcs = _srcs(),
        deps = [
            "//common:all_headers",
            "//soc-repo:all_headers",
        ],
        kernel_build = kernel_build,
        visibility = ["//visibility:public"],
    )

    copy_to_dist_dir(
        name = "{}_dist".format(rule_name),
        data = [rule_name],
        dist_dir = "out/target/product/{}/dlkm/lib/modules/".format(target),
        flat = True,
        wipe_dist_dir = False,
        allow_duplicate_filenames = False,
        mode_overrides = {"**/*": "644"},
        log = "info",
    )

def define_target_module(target):
    define_target_variant_module(target, "perf")
    define_target_variant_module(target, "consolidate")
    define_target_variant_module(target, "gki")
