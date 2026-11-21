/* Android bring-up: host services the Mac/iOS builds take from Apple frameworks
   or Husk's libslirp. Networking is net_slirp.c with libslirp; sound is audio_android.c;
   installing uses host/usb_host.c and the restore stack (android/build_restore.sh). */

#include <stdint.h>

#include "dns_proxy.h"

/* The guest's DNS through the host's resolver (host/dns_proxy.c) is DNSServiceQueryRecord, which Android has not:
   libslirp answers as before, through its own resolver (android/patches/libslirp-android-dns.patch). */
int hv_dns_proxy_on(void) { return 0; }
void hv_dns_proxy_start(void) {}
void hv_dns_proxy_stop(void) {}
int hv_dns_proxy_take(const uint8_t *p, uint32_t len, HvDnsSend send) { (void)p, (void)len, (void)send; return 0; }
