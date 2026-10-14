# Mesa's build tools on top of devkitPro's devkitA64 image (tools/mesa/build-mesa.sh).
FROM devkitpro/devkita64:latest
RUN apt-get update && apt-get install -y --no-install-recommends meson bison flex python3-mako xz-utils \
 && rm -rf /var/lib/apt/lists/*
