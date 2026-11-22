#ifndef HENGEVM_GUNYAH_H
#define HENGEVM_GUNYAH_H

/* Running the virtual Mac's CPUs on Qualcomm's Gunyah hypervisor (/dev/gunyah, the Android GH_* interface of
   android14-6.1 and later) from the jump into the kernel on. The machine is built exactly as for the JIT, and the
   JIT runs the boot ROM and iBoot (Gunyah gives a VM no memory where they live); at iBoot's jump into the kernel
   CPU 0 stops (host/vm.c, HV_HALT_HANDOVER) and this goes on from its state: Gunyah runs the guest's instructions on
   the real CPU and gives it its own GIC and timer; whatever the guest touches outside RAM comes back here as an MMIO
   exit and goes to Henge's devices, and their interrupt lines reach the guest through irqfds.

   Needs root (the device node's SELinux label keeps apps out), DroidVM's gunyah-host-share module, and the Mac's
   RAM all in 2 MB pages. Experimental: see gunyah.c for what is assumed. */

#define HV_GUNYAH_NOT_STARTED (-2) /* nothing of Gunyah's was started: Henge's CPUs can go on (hv_vm_resume_jit) */

/* Call once hv_vm_run_cpu0 returned with CPU 0 halted for the hand-over and hv_vm_end_jit ended the others.
   Runs until the guest powers off or resets, the VM fails, or hv_gunyah_stop(); returns an HV_EXIT_*-like reason
   (0 = powered off or stopped), or HV_GUNYAH_NOT_STARTED. */
int hv_gunyah_run(void);
void hv_gunyah_stop(void);

#endif
