#!/bin/sh
# Keep only the GPU firmware this machine can load, so the initramfs stays small.
set -e
TARGET_DIR="$1"
FW="$TARGET_DIR/lib/firmware/i915"
if [ -d "$FW" ]; then
	find "$FW" -type f ! -name 'kbl_*' -delete
	find "$FW" -type l ! -name 'kbl_*' -delete
fi

# Mesa's iris driver drags in clang and OpenCL pieces at build time (for its
# shader precompiler); at run time only libLLVM and libSPIRV-Tools are linked.
# Dropping the rest keeps the initramfs, which the kernel unpacks at every
# boot, about 120 MB smaller.
rm -f "$TARGET_DIR"/usr/lib/libclang.so* "$TARGET_DIR"/usr/lib/libclang-cpp.so* \
	"$TARGET_DIR"/usr/lib/libLLVMSPIRVLib.so* \
	"$TARGET_DIR"/usr/lib/libSPIRV-Tools-opt.so "$TARGET_DIR"/usr/lib/libSPIRV-Tools-link.so \
	"$TARGET_DIR"/usr/lib/libSPIRV-Tools-lint.so "$TARGET_DIR"/usr/lib/libSPIRV-Tools-diff.so \
	"$TARGET_DIR"/usr/lib/libSPIRV-Tools-reduce.so "$TARGET_DIR"/usr/lib/libSPIRV-Tools-shared.so
rm -rf "$TARGET_DIR"/usr/lib/clang
