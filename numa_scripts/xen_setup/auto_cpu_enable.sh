#!/bin/bash
cat > /etc/udev/rules.d/cpu-online.rules <<-EOF
SUBSYSTEM=="cpu", ACTION=="add", RUN+="/bin/sh -c 'echo 1 > %S%p/online'"
EOF
udevadm control --reload-rules
