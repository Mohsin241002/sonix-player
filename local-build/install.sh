#!/bin/sh
set -eu
: "${EXPECTED_MD5:?set EXPECTED_MD5 to the md5 of the image pushed as /mnt/sd_0/r1-candidate.upt}"
c=/mnt/sd_0/r1-candidate.upt
[ "$(grep -c 'HiBy R1' /usr/resource/sonix/components/system-info.json)" = 1 ]
[ "$(cat /sys/class/power_supply/battery/capacity)" -ge 50 ]
[ "$(md5sum "$c" | cut -d ' ' -f 1)" = "$EXPECTED_MD5" ]
[ ! -e /mnt/sd_0/r1.upt ]
# Retire the trial wrapper before its child so nothing restarts the player.
for f in /proc/[0-9]*/cmdline; do
 case "$(tr '\000' ' ' < "$f" 2>/dev/null)" in *sonix-trial/try.sh*) kill -KILL "$(echo $f | cut -d / -f 3)" 2>/dev/null || true;; esac
done
for p in $(pidof sonix_launch); do kill -KILL "$p"; done
for p in $(pidof sonix_player); do kill -TERM "$p"; done
sleep 3
for p in $(pidof sonix_player); do kill -KILL "$p"; done
umount /usr/resource/sonix/language 2>/dev/null || true
umount /usr/resource/sonix/fonts 2>/dev/null || true
umount /usr/bin/sonix_player 2>/dev/null || true
mv "$c" /mnt/sd_0/r1.upt
hwclock -w -u || true
sync
/usr/bin/bootmode.sh Recovery
sync
echo 'Recovery armed; rebooting to install'
sleep 1
reboot
