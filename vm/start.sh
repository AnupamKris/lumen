#!/bin/sh
# Boot the Debian 13 arm64 test VM headless.
#   SSH:  ssh -i vm/id_vm -p 2222 dev@127.0.0.1
#   VNC:  vnc://127.0.0.1:5905   (watch the desktop live)
#   QMP:  vm/qmp.sock            (screenshots + input injection, see vmctl.py)
cd "$(dirname "$0")"
exec qemu-system-aarch64 \
  -name lumen-dev \
  -M virt,highmem=on -accel hvf -cpu host -smp 4 -m 3072 \
  -bios /opt/homebrew/share/qemu/edk2-aarch64-code.fd \
  -drive if=virtio,file=disk.qcow2 \
  -drive if=virtio,format=raw,file=seed.iso,readonly=on \
  -device virtio-gpu-pci,xres=1440,yres=900 \
  -device virtio-keyboard-pci -device virtio-tablet-pci \
  -netdev user,id=n0,hostfwd=tcp:127.0.0.1:2222-:22 -device virtio-net-pci,netdev=n0 \
  -display none -vnc 127.0.0.1:5,password=on \
  -qmp unix:qmp.sock,server,nowait \
  -serial file:serial.log \
  -pidfile qemu.pid -daemonize
