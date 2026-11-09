#ifndef HENGEVM_GUNYAH_H
#define HENGEVM_GUNYAH_H

/* Running the virtual Mac's CPUs on Qualcomm's Gunyah hypervisor (/dev/gunyah, the Android GH_* interface of
   android14-6.1 and later) instead of the JIT. The machine is built exactly as for the JIT (hv_vm_create): guest
   RAM, the boot ROM and every device are the same. Gunyah runs the guest's instructions on the real CPU and gives
   it its own GIC and timer; whatever the guest touches outside RAM comes back here as an MMIO exit and goes to
   Henge's devices, and their interrupt lines reach the guest through irqfds.

   Needs root (the device node's SELinux label keeps apps out). Experimental: see gunyah.c for what is assumed. */

/* 0, or -1 with the reason on stderr. Runs until the guest powers off or resets, the VM fails, or
   hv_gunyah_stop(); returns an HV_EXIT_*-like reason (0 = powered off or stopped). */
int hv_gunyah_run(void);
void hv_gunyah_stop(void);

#endif
