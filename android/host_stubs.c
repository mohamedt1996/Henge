/* Android bring-up: host services the Mac/iOS builds take from Apple frameworks
   or Husk's libslirp. Networking is net_slirp.c with libslirp; no sound yet; USB host only matters when
   installing, which this build leaves to an iPhone or Mac. */
#include "audio_mac.h"
#include "usb_host.h"

void hv_audio_attach(HvVirtioSnd *d) { (void)d; }
void hv_libusb_shim_attach(HvVirtioUsb *d) { (void)d; }
void hv_usb_probe_start(HvVirtioUsb *d) { (void)d; }
