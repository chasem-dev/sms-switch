# Test bench for build/sms.nro: Xvfb plus screenshot/input tools; the
# emulator itself (an extracted Eden AppImage) is mounted at /emu.
FROM archlinux:latest
RUN pacman -Syu --noconfirm --needed xorg-server-xvfb xorg-xwd imagemagick xdotool \
      mesa vulkan-swrast libpulse alsa-lib fuse2 && pacman -Scc --noconfirm
