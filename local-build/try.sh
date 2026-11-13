#!/bin/sh
# Reversible trial: bind-mount the candidate over the installed player, run it
# under no supervisor, and restore the installed firmware's player on exit.
trial=/mnt/sd_0/.sonix-trial
[ "$(grep -c 'HiBy R1' /usr/resource/sonix/components/system-info.json)" = 1 ] || exit 1
/lib/ld.so.1 --list "$trial/sonix_player" > "$trial/loader.log" 2>&1 || exit 2
for p in $(pidof sonix_launch); do kill -KILL "$p"; done
for p in $(pidof sonix_player); do kill -TERM "$p"; done
sleep 2
for p in $(pidof sonix_player); do kill -KILL "$p"; done
mount -o bind "$trial/sonix_player" /usr/bin/sonix_player
mount -o bind "$trial/fonts" /usr/resource/sonix/fonts
mount -o bind "$trial/language" /usr/resource/sonix/language
ulimit -s 1024
export MALLOC_ARENA_MAX=1
/usr/bin/sonix_player > "$trial/player.log" 2>&1
echo "exit=$?" >> "$trial/player.log"
umount /usr/resource/sonix/language
umount /usr/resource/sonix/fonts
umount /usr/bin/sonix_player
exec /usr/bin/sonix_launch /usr/bin/sonix_player
