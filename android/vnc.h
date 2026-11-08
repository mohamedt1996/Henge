#ifndef HENGE_ANDROID_VNC_H
#define HENGE_ANDROID_VNC_H
/* Starts a VNC server thread on 127.0.0.1:port (all interfaces if HV_VNC_ANY is set). 0 or -1. */
int hv_vnc_start(int port);
#endif
