#!/bin/sh
# Screenshot the running Lumen session with grim and copy it to $1.
cd "$(dirname "$0")"
./ssh.sh 'XDG_RUNTIME_DIR=/run/user/1000 WAYLAND_DISPLAY=wayland-0 grim /tmp/g.png' &&
scp -q -P 2222 -i id_vm -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null -o LogLevel=ERROR dev@127.0.0.1:/tmp/g.png "$1"
