#!/system/bin/sh
# Henge: DroidVM's Gunyah host modules, loaded once Android has booted (the vendor Gunyah driver they reach into is
# loaded by then). A failed load is logged and left at that: nothing here retries or forces anything.
MODDIR=${0%/*}
LOG=/data/local/tmp/henge-gunyah.txt
# Henge 0.3.75: the Mac's RAM for Gunyah, taken now, while memory is still whole (Gunyah takes it only in 2 MB
# pages). The app turns this on (Settings > Gunyah memory > Reserve at boot) by leaving the size and its copy of
# henge-reserve in /data/adb/henge-gunyah; deleting reserve.conf turns it off.
CONF=/data/adb/henge-gunyah
if [ -f "$CONF/reserve.conf" ] && [ -x "$CONF/henge-reserve" ]; then
  MB=$(cat "$CONF/reserve.conf" 2>/dev/null | tr -cd 0-9)
  [ -n "$MB" ] && nohup "$CONF/henge-reserve" start "$MB" >/dev/null 2>&1 &
fi
until [ "$(getprop sys.boot_completed)" = 1 ]; do sleep 2; done
sleep 5
{
  echo "henge-gunyah $(date) kernel $(uname -r)"
  ls -l /dev/gunyah 2>&1
  for m in gunyah-kvcalloc-gki-6.1 gunyah-host-share-gki-6.1; do
    if [ -f "$MODDIR/$m.ko" ]; then
      insmod "$MODDIR/$m.ko" && echo "loaded $m" || echo "FAILED to load $m (exit $?)"
    fi
  done
  ls -l /dev/gunyah_share 2>&1
  dmesg 2>/dev/null | grep -iE "gunyah_share|gunyah_kvcalloc|ghk|gh_share|disagrees|Unknown symbol" | tail -20
} > "$LOG" 2>&1
chmod 644 "$LOG"
