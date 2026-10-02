#!/bin/zsh
# snapshot the kext fault properties the moment rc-count first goes up
mkdir -p ~/nvbuild/logs; rm -f ~/nvbuild/rc_first.txt
for i in $(seq 1 600); do
  c=$(ioreg -rc NVGspControl -l -w0 | grep -oE "\"NVGspControl-rc-count\" = [0-9]+" | grep -oE "[0-9]+$")
  if [ "${c:-0}" != "0" ]; then
    sleep 0.3   # the Xid lines of one RC come in a burst
    ioreg -rc NVGspControl -l -w0 > ~/nvbuild/logs/rc_ioreg.txt
    grep -oE "\"NVGspControl-(fault-owner|rc-[a-z-]*|mmu-fault-events|stall[a-z-]*|gr-hung|ce-hung|autoreset-[a-z-]*|os-error-last-type)\" = (\"[^\"]*\"|[0-9a-f<>]+|Yes|No)" ~/nvbuild/logs/rc_ioreg.txt > ~/nvbuild/rc_first.txt
    grep -oE "\"NVGspControl-os-error-log\" = .*" ~/nvbuild/logs/rc_ioreg.txt | cut -c1-1200 >> ~/nvbuild/rc_first.txt
    exit 0
  fi
  sleep 0.05
done
