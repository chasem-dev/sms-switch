# The build image: devkitPro's devkitA64 image plus ccache, so rebuilds after
# ./build.sh --clean or a header change reuse unchanged objects. build.sh
# builds it as sms-switch-build the first time it is needed.
FROM devkitpro/devkita64:latest
RUN apt-get update && apt-get install -y --no-install-recommends ccache \
 && rm -rf /var/lib/apt/lists/*
