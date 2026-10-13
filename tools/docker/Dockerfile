# The build image: devkitPro's devkitA64 image plus ccache, so rebuilds after
# ./build.sh --clean or a header change reuse unchanged objects, and Pillow
# for the icon made from your disc (tools/make_icon.py). build.sh builds it as
# sms-switch-build:<hash of this file> whenever this file changes.
FROM devkitpro/devkita64:latest
RUN apt-get update && apt-get install -y --no-install-recommends ccache python3-pil \
 && rm -rf /var/lib/apt/lists/*
