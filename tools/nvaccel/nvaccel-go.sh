#!/bin/sh
# Boot-time start of NVAccelerator (B4) under a file-based crash guard.
# Installed as /usr/local/libexec/nvaccel-go.sh, run by com.macoslab.nvaccel-go.
#   enable:  sudo touch /usr/local/share/nvgsp/accel.enable
#   disable: sudo rm /usr/local/share/nvgsp/accel.enable
# /var/db/nvaccel.pending exists from the go until 300 s later; finding it at
# boot means the last start did not survive (panic/reset): skip this boot.
exec >>/var/log/nvaccel-go.log 2>&1
EN=/usr/local/share/nvgsp/accel.enable
P=/var/db/nvaccel.pending
echo "=== nvaccel-go $(date)"
[ -f "$EN" ] || { echo "disabled (no $EN)"; exit 0; }
if [ -f "$P" ]; then
  echo "previous accelerator start did not survive: skipping this boot"
  rm -f "$P"; sync; exit 0
fi
i=0
until /usr/sbin/ioreg -rc NVGspControl -l | grep -q '"NVGspControl-gr-persistent" = Yes'; do
  sleep 2; i=$((i+1)); [ $i -gt 300 ] && { echo "GSP chain not ready, giving up"; exit 1; }
done
touch "$P"; sync
if /usr/local/libexec/nvaccel_go; then echo "go sent $(date)"; else echo "go failed"; rm -f "$P"; sync; exit 1; fi
# 29 Sep: WindowServer looks for a Metal device once, at start. It is up
# long before the accelerator (the GSP chain waits for the login window), so
# it settles on the software compositor ("Failed to create MetalDevice for
# accelerator 0") and never looks again. At the login window, with nobody
# logged in, restart it once the accelerator is registered: loginwindow
# brings it straight back, now on the Metal compositor (one blink).
# /usr/local/share/nvgsp/no-ws-restart turns this off.
if [ ! -f /usr/local/share/nvgsp/no-ws-restart ]; then
  i=0
  until /usr/sbin/ioreg -rc NVAccelerator -d0 | grep -q NVAccelerator; do
    sleep 0.5; i=$((i+1)); [ $i -gt 40 ] && break
  done
  if /usr/bin/who | grep -q console; then
    echo "user logged in: WindowServer left as it is"
  else
    sleep 1
    # SIGTERM is ignored; loginwindow relaunches it after a KILL
    old=$(/usr/bin/pgrep -x WindowServer)
    /usr/bin/killall -KILL WindowServer
    i=0
    until new=$(/usr/bin/pgrep -x WindowServer) && [ "$new" != "$old" ]; do
      sleep 0.5; i=$((i+1)); [ $i -gt 40 ] && break
    done
    echo "WindowServer $old -> ${new:-none} for the Metal compositor $(date)"
    # 1 Oct: optional login right after the switch, so a test session always
    # starts on the Metal compositor (boot-time automatic login would start it
    # on the software one). /usr/local/share/nvgsp/autologin turns it on;
    # lwlogin types the /etc/kcpassword password through a virtual keyboard.
    # 1 Oct 14:42: the first try can type before the password field takes
    # keys (typed 4 of 4, no session); a second run then works. Up to three
    # tries, 20 s apart, until a console session shows up.
    if [ -f /usr/local/share/nvgsp/autologin ] && [ -x /usr/local/libexec/lwlogin ]; then
      sleep 4
      for try in 1 2 3; do
        /usr/local/libexec/lwlogin
        i=0
        until /usr/bin/who | grep -q console; do sleep 0.5; i=$((i+1)); [ $i -gt 40 ] && break; done
        /usr/bin/who | grep -q console && break
        echo "login try $try: no console session yet"
      done
      echo "login: $(/usr/bin/who | grep console | cut -c1-40)"
    fi
  fi
fi
sleep 300
rm -f "$P"; sync
echo "accelerator survived 300 s $(date)"
