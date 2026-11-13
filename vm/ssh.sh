#!/bin/sh
cd "$(dirname "$0")" && exec ssh -q -i id_vm -p 2222 -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null -o LogLevel=ERROR dev@127.0.0.1 "$@"
