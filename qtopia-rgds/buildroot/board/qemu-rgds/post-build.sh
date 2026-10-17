#!/bin/sh
# Copy a Qt Extended image (bin/qbuild image output) into /opt/qtmoko.
# QTEXTENDED_IMAGE=<build dir>/image make ...
set -e
TARGET_DIR=$1
if [ -n "$QTEXTENDED_IMAGE" ] && [ -d "$QTEXTENDED_IMAGE" ]; then
    rm -rf "$TARGET_DIR/opt/qtmoko"
    mkdir -p "$TARGET_DIR/opt"
    cp -a "$QTEXTENDED_IMAGE" "$TARGET_DIR/opt/qtmoko"
fi
