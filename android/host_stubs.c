/* Android bring-up: host services the Mac/iOS builds take from Apple frameworks
   or Husk's libslirp. Networking is net_slirp.c with libslirp; no sound yet;
   installing uses host/usb_host.c and the restore stack (android/build_restore.sh). */
#include "audio_mac.h"

void hv_audio_attach(HvVirtioSnd *d) { (void)d; }
