#!/bin/bash

wget https://github.com/ImagingDataCommons/libdicom/releases/download/v1.2.0/libdicom-1.2.0.tar.xz -O - | tar xf -

meson setup builddir
meson compile -C builddir
meson install -C builddir
