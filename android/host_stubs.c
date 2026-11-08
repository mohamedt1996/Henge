/* Android bring-up: host services the Mac/iOS builds take from Apple frameworks
   or Husk's libslirp. No network and no sound yet; USB host only matters when
   installing, which this build leaves to an iPhone or Mac. */
#include "audio_mac.h"
#include "net_slirp.h"
#include "usb_host.h"

void hv_audio_attach(HvVirtioSnd *d) { (void)d; }
void hv_net_set_share_socket(const char *path) { (void)path; }
int hv_net_attach(HvMachine *m, HvVirtioNet *n) { (void)m; (void)n; return -1; }
void hv_net_poll(void) {}
void hv_libusb_shim_attach(HvVirtioUsb *d) { (void)d; }
void hv_usb_probe_start(HvVirtioUsb *d) { (void)d; }
