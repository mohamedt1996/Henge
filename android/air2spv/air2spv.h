/* SPDX-License-Identifier: GPL-2.0-or-later */
/* air2spv: Metal AIR (one function's LLVM bitcode, as macOS hands it to the paravirtual GPU)
 * -> SPIR-V for the Android Vulkan replay. ABI.md describes where each Metal resource ends up. */
#ifndef AIR2SPV_H
#define AIR2SPV_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define AIR2SPV_ABI_VERSION 2

/* Push-constant block: one u64 buffer address per slot, 16 slots a stage. The vertex (and kernel) stage
   uses slots 0-15, the fragment stage 16-31: 256 bytes in all. */
#define AIR2SPV_PUSH_SLOTS 16
#define AIR2SPV_PUSH_BYTES (AIR2SPV_PUSH_SLOTS * 8 * 2)
#define AIR2SPV_CONST_DATA_SLOT 15 /* the function's constant tables */

/* Bindings, in set 0 for fragment and kernel functions and set 1 for vertex functions. */
#define AIR2SPV_BINDING_TEXTURE 0  /* + Metal texture(n), sampled or read */
#define AIR2SPV_BINDING_SAMPLER 16 /* + S# n (API sampler(n) or constexpr sampler slot) */
#define AIR2SPV_BINDING_IMAGE 32   /* + Metal texture(n), access::write */
#define AIR2SPV_BINDING_FBFETCH 48 /* + colour attachment n read in the shader (input attachment n) */

struct air2spv_constant {  /* a function constant set by the pipeline */
  int index;               /* by index, or -1 to use name */
  const char *name;
  const double *values;
  unsigned count;
};

struct air2spv_options {
  const char *entry;        /* NULL: the module's single entry point */
  int stage;                /* -1 any, 0 vertex, 4 fragment, 5 kernel (mesa_shader_stage) */
  const struct air2spv_constant *constants;
  unsigned num_constants;
  uint8_t color_format_class[8]; /* air2ir3 format classes; 0 = native */
  uint8_t texture_class[16];
};

struct air2spv_result {
  uint32_t *words;       /* SPIR-V */
  size_t num_words;
  uint8_t *constant_data; /* bytes to place at the push slot AIR2SPV_CONST_DATA_SLOT's address */
  size_t constant_data_size;
  char *json;            /* sidecar: reflection, descriptors, push slots */
  char *error;           /* set when the function returns non-zero */
};

int air2spv_compile(const void *air, size_t size, const struct air2spv_options *opts, struct air2spv_result *out);
void air2spv_result_free(struct air2spv_result *r);

#ifdef __cplusplus
}
#endif
#endif
