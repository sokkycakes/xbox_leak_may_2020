#!/bin/sh
# Link a Buildroot output dir to the fixed paths the rgds mkspec uses.
# usage: setup-sdk.sh <buildroot output dir>
set -e
out=$(readlink -f "$1")
mkdir -p /opt/rgds-sdk
ln -sfn "$out/host" /opt/rgds-sdk/host
ln -sfn "$(readlink -f "$out/staging")" /opt/rgds-sdk/sysroot
