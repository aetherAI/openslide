FROM debian:bookworm-slim

# Install system dependencies for building openslide and libdicom
RUN apt update \
    && apt install -y --no-install-recommends \
        meson \
        gcc \
        g++ \
        make \
        libpng-dev \
        libjpeg-dev \
        libtiff-dev \
        libopenjp2-7-dev \
        libsqlite3-dev \
        libgdk-pixbuf2.0-dev \
        libxml2-dev \
        libcairo2-dev \
        libzstd-dev \
    && rm -rf /var/cache/apt/archives /var/lib/apt/lists/*

# Default working directory
WORKDIR /openslide

# Default command
CMD ["./compile.sh"]
