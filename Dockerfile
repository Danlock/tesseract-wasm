FROM buildpack-deps:22.04


# https://tesseract-ocr.github.io/tessdoc/Compiling.html
RUN set -eux; \
    apt-get update; \
    apt-get install -y --no-install-recommends \
    cmake \
    ninja-build \
    ; rm -rf /var/lib/apt/lists/*

RUN ls -la /usr/lib
RUN ls -la /usr/include