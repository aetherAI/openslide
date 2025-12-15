FROM debian:bookworm-slim

# Install system dependencies for building openslide and libdicom
RUN apt update \
    && apt install -y --no-install-recommends \
        meson \
        gcc \
        g++ \
        make \
        wget \
        ca-certificates \
        libpng-dev \
        libjpeg-dev \
        libtiff-dev \
        libopenjp2-7-dev \
        libsqlite3-dev \
        libgdk-pixbuf2.0-dev \
        libxml2-dev \
        libcairo2-dev \
        libzstd-dev \
        libhwy-dev \
    && rm -rf /var/cache/apt/archives /var/lib/apt/lists/*

# Install libjxl from precompiled deb packages
RUN mkdir jxl-debs \
    && cd jxl-debs \
    && wget https://github.com/libjxl/libjxl/releases/download/v0.11.1/jxl-debs-amd64-debian-bookworm-v0.11.1.tar.gz -O - \
    | tar -xzf - \
    && dpkg -i libjxl_0.11.1_amd64.deb libjxl-dev_0.11.1_amd64.deb \
    && cd .. \
    && rm -rf jxl-debs

# Default working directory
WORKDIR /openslide

# Default command
CMD ["./compile.sh"]
