/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Graphics: reims, the app's half (host.c). One host per process, for the VM the service runs. */
#ifndef HV_REIMS_HOST_H
#define HV_REIMS_HOST_H

typedef struct Replay Replay;

/* Waits for the engine's file at path (gpu.reims next to the disk) and brings reims-vgpu up on it. */
int reims_host_start(const char *path, const char *log_path);
void reims_host_stop(void);
int reims_host_running(void);
/* The screen frames go to (NULL: none; frames are then not drawn). */
void reims_host_set_sink(Replay *r);

#endif
