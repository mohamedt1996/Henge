# Henge engine for Android/Linux arm64 (headless bring-up: no GPU, serial console only).
#   make -f android/Makefile            -> build/android/{libhengevm.a, test_*}
#   make -f android/Makefile test       -> runs the tests (under qemu-aarch64 when not on arm64)
# With the NDK: make -f android/Makefile CC="$NDK/toolchains/llvm/prebuilt/linux-x86_64/bin/aarch64-linux-android29-clang" STATIC=
CC ?= clang
ifeq ($(CC),cc)
CC = clang
endif
TARGET ?= --target=aarch64-linux-gnu
CFLAGS = $(TARGET) -march=armv8.2-a+crypto -D_GNU_SOURCE -std=gnu11 -O2 -g -w -Iandroid/compat -Iandroid -Icore -Ihost -Idevices
ASFLAGS = $(TARGET) -march=armv8.6-a+crypto
STATIC ?= -static
LIBS = $(STATIC) -lpthread -lm
B = build/android
A = $(B)/asm
DEPS ?= $(HOME)/deps/arm64
SLIRP_CFLAGS = $(shell PKG_CONFIG_LIBDIR=$(DEPS)/lib/pkgconfig pkg-config --cflags slirp)
SLIRP_LIBS = $(shell PKG_CONFIG_LIBDIR=$(DEPS)/lib/pkgconfig pkg-config --static --libs slirp)
RUN ?= $(if $(filter aarch64 arm64,$(shell uname -m)),,qemu-aarch64 -cpu max)

CORE = classify operands translate dispatch mmu memop exception sysreg machine
DEVS = $(basename $(notdir $(wildcard devices/*.c)))
ENGINE = $(CORE:%=$(B)/%.o) $(B)/jit_region.o $(B)/disk.o $(B)/enter.o
DEV = $(DEVS:%=$(B)/%.o) $(B)/pvg_android.o
ASM = $(A)/harness.h $(A)/harness.S $(patsubst tests/%,$(A)/%,$(wildcard tests/*_programs.S tests/*_cases.s))
TESTS = $(B)/test_classify $(B)/test_operands $(B)/test_jit_region $(B)/test_mmu $(B)/test_jit $(B)/test_mem $(B)/test_exc $(B)/test_devices

all: $(B)/libhengevm.a $(B)/hengevm-android $(TESTS)

$(B) $(A):
	mkdir -p $@

# Mach-O assembly -> ELF (symbol underscores, @PAGE relocations).
$(A)/%: tests/% android/macho2elf.sh | $(A)
	android/macho2elf.sh $< > $@
$(A)/enter.S: host/enter.S android/macho2elf.sh | $(A)
	android/macho2elf.sh $< > $@
$(B)/enter.o: $(A)/enter.S | $(B)
	$(CC) $(ASFLAGS) -c $< -o $@

$(B)/mmu.o: core/mmu.c core/*.h host/*.h | $(B)
	$(CC) $(CFLAGS) -mgeneral-regs-only -c $< -o $@
$(B)/%.o: core/%.c core/*.h host/*.h android/compat/plat_linux.h | $(B)
	$(CC) $(CFLAGS) -c $< -o $@
$(B)/%.o: host/%.c host/*.h | $(B)
	$(CC) $(CFLAGS) -c $< -o $@
$(B)/%.o: devices/%.c devices/*.h android/compat/aes_soft.h | $(B)
	$(CC) $(CFLAGS) -c $< -o $@
$(B)/pvg_android.o: android/pvg_android.c | $(B)
	$(CC) $(CFLAGS) -c $< -o $@
# Linked as an object, not from the archive: it overrides vmapple.c's weak hv_pvg_external_create.
$(B)/pvg_applegfx.o: android/pvg_applegfx.c | $(B)
	$(CC) $(CFLAGS) -c $< -o $@
$(B)/pvg_reims.o: android/pvg_reims.c android/gpu/reims_link.h android/gpu/ram_fd.h | $(B)
	$(CC) $(CFLAGS) -c $< -o $@

$(B)/vm.o: host/vm.c host/*.h android/compat/plat_linux.h | $(B)
	$(CC) $(CFLAGS) -c $< -o $@
$(B)/host_stubs.o: android/host_stubs.c | $(B)
	$(CC) $(CFLAGS) -c $< -o $@
$(B)/net_slirp.o: host/net_slirp.c host/*.h | $(B)
	$(CC) $(CFLAGS) $(SLIRP_CFLAGS) -c $< -o $@
$(B)/audio_android.o: android/audio_android.c host/audio_mac.h devices/virtio_snd.h | $(B)
	$(CC) $(CFLAGS) -c $< -o $@
$(B)/vnc.o: android/vnc.c android/vnc.h host/vm.h | $(B)
	$(CC) $(CFLAGS) -c $< -o $@
LZFSE ?= $(dir $(DEPS))src/lzfse/src
LZFSE_OBJ = $(B)/lzfse_decode.o $(B)/lzfse_decode_base.o $(B)/lzfse_fse.o $(B)/lzvn_decode_base.o
$(B)/lzfse_%.o: $(LZFSE)/lzfse_%.c | $(B)
	$(CC) $(CFLAGS) -c $< -o $@
$(B)/lzvn_decode_base.o: $(LZFSE)/lzvn_decode_base.c | $(B)
	$(CC) $(CFLAGS) -c $< -o $@
$(B)/curl_android.o: android/curl_android.c | $(B)
	$(CC) $(CFLAGS) -I$(DEPS)/include -c $< -o $@
$(B)/fetch.o: android/fetch.c android/fetch.h | $(B)
	$(CC) $(CFLAGS) -I$(DEPS)/include -I$(LZFSE) -c $< -o $@
$(B)/seccomp_compat.o: android/seccomp_compat.c | $(B)
	$(CC) $(CFLAGS) -c $< -o $@
$(B)/runner.o: android/runner.c android/*.h host/vm.h | $(B)
	$(CC) $(CFLAGS) -I$(DEPS)/include -c $< -o $@
# Root-only check of the tablet's Gunyah hypervisor (the app's device check runs it through su).
$(B)/gunyah-probe: android/tools/gunyah_probe.c android/tools/gunyah_probe_blobs.h | $(B)
	$(CC) $(TARGET) -O2 -Iandroid/tools $< $(STATIC) -o $@
$(B)/libhengerestore.a: android/build_restore.sh host/install.c host/libusb_shim.c
	DEPS=$(DEPS) android/build_restore.sh
RESTORE_LIBS = $(B)/libhengerestore.a $(DEPS)/libzip/lib/libzip.a -L$(DEPS)/lib -lcurl -lmbedtls -lmbedx509 -lmbedcrypto -lz
$(B)/hengevm-android: $(B)/seccomp_compat.o $(B)/runner.o $(B)/gunyah.o $(B)/pvg_reims.o $(B)/pvg_applegfx.o $(B)/curl_android.o $(B)/fetch.o $(LZFSE_OBJ) $(B)/vm.o $(B)/usb_host.o $(B)/net_slirp.o $(B)/vnc.o $(B)/audio_android.o $(B)/host_stubs.o $(B)/libhengevm.a $(B)/libhengerestore.a
	$(CC) $(CFLAGS) $(filter-out $(B)/libhengerestore.a,$^) $(RESTORE_LIBS) $(SLIRP_LIBS) $(LIBS) -Wl,--wrap=curl_easy_init -o $@
	python3 android/nosyscall.py $@

$(B)/libhengevm.a: $(ENGINE) $(DEV)
	rm -f $@
	llvm-ar rcs $@ $^ 2>/dev/null || ar rcs $@ $^

T = $(CC) $(CFLAGS) $(ASFLAGS) -I$(A) -Itests
$(B)/test_classify: tests/test_classify.c $(A)/classify_cases.s $(B)/classify.o
	$(T) $^ $(LIBS) -o $@
$(B)/test_operands: tests/test_operands.c $(A)/operand_cases.s $(B)/operands.o
	$(T) $^ $(LIBS) -o $@
$(B)/test_jit_region: tests/test_jit_region.c $(B)/jit_region.o
	$(T) $^ $(LIBS) -o $@
$(B)/test_mmu: tests/test_mmu.c $(ENGINE)
	$(T) $^ $(LIBS) -o $@
$(B)/test_devices: tests/test_devices.c $(ASM) $(ENGINE) $(DEV)
	$(T) tests/test_devices.c $(A)/devices_programs.S $(A)/harness.S $(ENGINE) $(DEV) $(LIBS) -o $@
$(B)/test_%: tests/test_%.c $(ASM) $(ENGINE)
	$(T) tests/test_$*.c $(A)/$*_programs.S $(A)/harness.S $(ENGINE) $(LIBS) -o $@

test: $(TESTS)
	@mkdir -p /tmp/hengevm-test; ulimit -s 65520; fail=0; for t in $(TESTS); do $(RUN) $$t > $$t.log 2>&1 || fail=1; echo "$$t: $$(tail -1 $$t.log)"; done; exit $$fail

clean:
	rm -rf $(B)

.PHONY: all test clean
