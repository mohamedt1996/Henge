import struct, sys
# Minimal flattened device tree for the probe VM (and the gunyah-vm-config node the resource manager reads).
BASE, SIZE = 0x80000000, 0x200000
strings = bytearray(); offs = {}
def soff(n):
    if n not in offs: offs[n] = len(strings); strings.extend(n.encode() + b'\0')
    return offs[n]
st = bytearray()
def u32(*v): return b''.join(struct.pack('>I', x) for x in v)
def begin(n): st.extend(u32(1) + n.encode() + b'\0'); st.extend(b'\0' * (-len(st) % 4))
def end(): st.extend(u32(2))
def prop(n, v):
    if isinstance(v, str): v = v.encode() + b'\0'
    st.extend(u32(3, len(v), soff(n)) + v); st.extend(b'\0' * (-len(st) % 4))
GIC = 1
begin('')
prop('#address-cells', u32(2)); prop('#size-cells', u32(2))
prop('compatible', 'linux,dummy-virt'); prop('interrupt-parent', u32(GIC))
begin('memory@80000000'); prop('device_type', 'memory'); prop('reg', u32(0, BASE, 0, SIZE)); end()
begin('cpus'); prop('#address-cells', u32(1)); prop('#size-cells', u32(0))
begin('cpu@0'); prop('device_type', 'cpu'); prop('compatible', 'arm,armv8'); prop('reg', u32(0)); prop('enable-method', 'psci'); end()
end()
begin('psci'); prop('compatible', 'arm,psci-1.0'); prop('method', 'hvc'); end()
begin('intc@3fff0000'); prop('compatible', 'arm,gic-v3'); prop('#interrupt-cells', u32(3)); prop('interrupt-controller', b'')
prop('#address-cells', u32(2)); prop('#size-cells', u32(2))
prop('reg', u32(0, 0x3fff0000, 0, 0x10000, 0, 0x3ffd0000, 0, 0x20000)); prop('phandle', u32(GIC)); end()
begin('timer'); prop('compatible', 'arm,armv8-timer')
prop('interrupts', u32(1, 13, 0x108, 1, 14, 0x108, 1, 11, 0x108, 1, 10, 0x108)); prop('always-on', b''); end()
begin('gunyah-vm-config'); prop('image-name', 'henge-probe'); prop('os-type', 'linux')
begin('memory'); prop('#address-cells', u32(2)); prop('#size-cells', u32(2)); prop('base-address', u32(0, BASE)); end()
begin('interrupts'); prop('config', u32(GIC)); end()
begin('vcpus'); prop('affinity', 'proxy'); end()
begin('vdevices'); prop('generate', '/hypervisor'); end()
end()
end(); st.extend(u32(9))
rsv = u32(0, 0, 0, 0)
hdr_len = 40
off_rsv = hdr_len; off_st = off_rsv + len(rsv); off_str = off_st + len(st)
total = off_str + len(strings)
hdr = u32(0xd00dfeed, total, off_st, off_str, off_rsv, 17, 16, 0, len(strings), len(st))
sys.stdout.buffer.write(hdr + rsv + bytes(st) + bytes(strings))
