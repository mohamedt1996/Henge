#pragma once
/* The one CoreFoundation use outside Apple-only files: retaining an opaque display layer. */
typedef const void *CFTypeRef;
static inline CFTypeRef CFRetain(CFTypeRef p) { return p; }
static inline void CFRelease(CFTypeRef p) { (void)p; }
