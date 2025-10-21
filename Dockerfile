# Start from the same base you use
FROM quay.io/pypa/manylinux_2_28_x86_64

# Install system dependencies for building openslide and libdicom
RUN yum install -y \
        meson \
        gcc \
        gcc-c++ \
        make \
        libpng-devel \
        libjpeg-devel \
        libtiff-devel \
        openjpeg2-devel \
        gdk-pixbuf2-devel \
        libxml2-devel \
        sqlite-devel \
        cairo-devel \
        libzstd-devel \
    && yum clean all \
    && rm -rf /var/cache/yum

# Default working directory
WORKDIR /openslide-pybind11

# Default command
CMD ["./compile.sh"]
