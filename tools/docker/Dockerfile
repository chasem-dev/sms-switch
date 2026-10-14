# The build image: devkitPro's devkitA64 image plus ccache, so rebuilds after
# ./build.sh --clean or a header change reuse unchanged objects, and Pillow
# for the icon (tools/make_icon.py), clang for Super Mario Eclipse's code
# (sms-eclipse.nro: its sources are written for clang; llvm for the port's
# llvm-dwarfdump check of it), and hacBrewPack for the installable NSP
# (tools/make-nsp.sh, rlaphoenix's maintained fork). build.sh builds it as
# sms-switch-build:<hash of this file> whenever this file changes.
FROM devkitpro/devkita64:latest
RUN apt-get update && apt-get install -y --no-install-recommends ccache python3-pil clang-19 llvm-19 \
 && rm -rf /var/lib/apt/lists/*
ARG HACBREWPACK_COMMIT=745b16e
RUN git clone -q https://github.com/rlaphoenix/hacBrewPack.git /tmp/hacbrewpack \
 && cd /tmp/hacbrewpack && git checkout -q $HACBREWPACK_COMMIT \
 && cp config.mk.template config.mk && make -s >/dev/null \
 && install -m755 hacbrewpack /usr/local/bin/ && rm -rf /tmp/hacbrewpack
