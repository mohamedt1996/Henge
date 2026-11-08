// SPDX-License-Identifier: GPL-2.0-or-later
// air2spv: Metal AIR -> NIR (air2ir3's front end, third_party/air2ir3, MIT) -> SPIR-V (Mesa's
// zink nir_to_spirv, MIT).  The NIR the front end produces follows air2ir3's Adreno binding ABI
// (UBO n, a driver table of device-buffer addresses, T#/S# indices, framebuffer fetch from output
// variables); the passes here move it onto the Vulkan layout in ABI.md before nir_to_spirv.
//
// One translation unit, like air2ir3's own src/air2ir3.cpp: the front-end parts are #included in
// the order that file includes them.
#include "llvm/ADT/DepthFirstIterator.h"
#include "llvm/ADT/PostOrderIterator.h"
#include "llvm/ADT/APSInt.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/Analysis/ConstantFolding.h"
#include "llvm/Analysis/InstructionSimplify.h"
#include "llvm/Analysis/PostDominators.h"
#include "llvm/Analysis/ValueTracking.h"
#include "llvm/IR/ConstantRange.h"
#include "llvm/Analysis/TargetLibraryInfo.h"
#include "llvm/IR/Dominators.h"
#include "llvm/Transforms/Utils/Local.h"
#include "llvm/IR/CFG.h"
#include "llvm/Bitcode/BitcodeReader.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/DataLayout.h"
#include "llvm/IR/GetElementPtrTypeIterator.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/IntrinsicInst.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Operator.h"
#include "llvm/IR/Verifier.h"
#include "llvm/Passes/PassBuilder.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/SHA256.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Transforms/Utils/Cloning.h"
#include "llvm/Transforms/Utils/Evaluator.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

#include "nir.h"
#include "nir_builder.h"
#include "nir_builtin_builder.h"
#include "glsl_types.h"
#include "util/blob.h"
#include "a8xx_sampler.h"
#include "air2ir3.h"
extern "C" {
#include "nir_to_spirv/nir_to_spirv.h"
}
#include "air2spv.h"

using namespace llvm;

#ifdef AIR2SPV_MESA_RELEASE
// A release (NDEBUG) Mesa has no NIR debug flags; the front end still sets them.
extern "C" { uint32_t nir_debug = 0; void nir_process_debug_variable(void) {} }
#endif

#include "air2ir3/abi_metadata.cpp"
#include "air2ir3/fc_ctor_eval.cpp"
#include "air2ir3/unset_body.cpp"
#include "air2ir3/specialize.cpp"
#include "air2ir3/translator.cpp"
#include "air2ir3/translator_rt.cpp"
#include "air2ir3/translator_args.cpp"
#include "air2ir3/translator_calls.cpp"
#include "air2ir3/translator_insts.cpp"
#include "air2ir3/frontend.cpp"

// The sidecar's value of a function constant, as air2ir3's library.cpp writes it (MIT, Snapintosh
// contributors): exact in the declared type, floats with their bit patterns.
static std::string fcValueJson(const AirFc &f) {
  unsigned n = 1; std::string base = fcScalarBase(f.type, n);
  bool fp = base == "float" || base == "half";
  std::string v = "[", bits = "[";
  for (double x : f.value) {
    if (v.size() > 1) { v += ","; bits += ","; }
    char b[64];
    if (!fp) {
      if (base == "ulong" || base == "uint" || base == "ushort" || base == "uchar" || base == "bool") snprintf(b, sizeof b, "%llu", (unsigned long long)fcIntBits(x));
      else snprintf(b, sizeof b, "%lld", (long long)(int64_t)fcIntBits(x));
      v += b; continue;
    }
    if (std::isnan(x)) v += std::signbit(x) ? "\"-nan\"" : "\"nan\"";
    else if (std::isinf(x)) v += x < 0 ? "\"-inf\"" : "\"inf\"";
    else if (x == 0) v += std::signbit(x) ? "-0.0" : "0.0";
    else {
      for (int prec = 1; prec <= 17; prec++) {
        snprintf(b, sizeof b, "%.*g", prec, x);
        if ((float)strtod(b, nullptr) == (float)x) break;
      }
      std::string s = b;
      if (s.find_first_of(".en") == std::string::npos) s += ".0";
      v += s;
    }
    if (base == "float") { float y = (float)x; uint32_t u; memcpy(&u, &y, 4); snprintf(b, sizeof b, "\"0x%08x\"", u); }
    else { _Float16 y = (_Float16)x; uint16_t u; memcpy(&u, &y, 2); snprintf(b, sizeof b, "\"0x%04x\"", u); }
    bits += b;
  }
  return "\"value\":" + v + "]" + (fp ? ",\"bits\":" + bits + "]" : std::string());
}

// ------------------------------------------------------------------------------ NIR options --
// Mesa zink's defaults (zink_screen_init_compiler) for a Vulkan 1.3 device with shaderInt64,
// shaderFloat16 and demote, no shaderFloat64 (Adreno 7xx).
static const nir_shader_compiler_options *spvNirOptions() {
  static nir_shader_compiler_options o = [] {
    nir_shader_compiler_options x = {};
    x.lower_scmp = true;
    x.lower_fdph = true;
    x.lower_flrp32 = true;
    x.lower_flrp64 = true;
    x.lower_fsat = true;
    x.lower_hadd = true;
    x.lower_iadd_sat = true;
    x.lower_fisnormal = true;
    x.lower_extract_byte = true;
    x.lower_extract_word = true;
    x.lower_insert_byte = true;
    x.lower_insert_word = true;
    x.has_ldexp = false;
    x.lower_mul_high = true;
    x.lower_to_scalar = true;
    x.lower_uadd_carry = true;
    x.compact_arrays = true;
    x.lower_usub_borrow = true;
    x.lower_uadd_sat = true;
    x.lower_usub_sat = true;
    x.lower_int64_options = (nir_lower_int64_options)(nir_lower_bit_count64 | nir_lower_find_lsb64 | nir_lower_ufind_msb64 |
                                                      nir_lower_bitfield_reverse64 | nir_lower_bitfield_extract64);
    x.lower_doubles_options = (nir_lower_doubles_options)~0;
    x.has_fsub = true;
    x.has_isub = true;
    x.lower_mul_2x32_64 = true;
    x.support_16bit_alu = true;
    x.max_unroll_iterations = 0;
    x.float_mul_add32 = nir_float_muladd_support_keep_weak_ffma;
    x.float_mul_add16 = nir_float_muladd_support_keep_weak_ffma;
    x.discard_is_demote = true;
    x.lower_bitfield_extract16 = true;
    x.lower_bitfield_extract8 = true;
    x.support_indirect_inputs = BITFIELD_BIT(MESA_SHADER_FRAGMENT);
    x.support_indirect_outputs = (uint8_t)BITFIELD_MASK(MESA_SHADER_STAGES);
    return x;
  }();
  return &o;
}

// ------------------------------------------------------------------------- Vulkan lowering --
struct SpvLayout {
  std::map<int, nir_variable *> tex, smp, img, fb;     // by T# / S# / image / colour attachment
  int maxPushSlot = -1;
  bool usesConstData = false;
};

static nir_def *pushAddr(nir_builder *b, int slot) {
  nir_def *w = nir_load_push_constant_zink(b, 2, 32, nir_imm_int(b, 0), .component = (unsigned)slot * 2);
  return nir_pack_64_2x32(b, w);
}

// load_ubo(n, off) -> load_global(push[n] + off); load_constant(off) -> load_global(push[15] + base + off).
static bool lowerBuffers(nir_shader *s, SpvLayout &L) {
  return nir_shader_intrinsics_pass(s, [](nir_builder *b, nir_intrinsic_instr *in, void *d) -> bool {
    SpvLayout &L = *(SpvLayout *)d;
    if (in->intrinsic != nir_intrinsic_load_ubo && in->intrinsic != nir_intrinsic_load_constant) return false;
    b->cursor = nir_before_instr(&in->instr);
    int slot; nir_def *off;
    if (in->intrinsic == nir_intrinsic_load_ubo) {
      if (!nir_src_is_const(in->src[0])) die("air2spv: dynamically indexed constant buffer");
      slot = (int)nir_src_as_uint(in->src[0]);
      if (slot >= AIR2SPV_CONST_DATA_SLOT)
        die("constant buffer / driver table at index " + std::to_string(slot) + ": push slots 0.." +
            std::to_string(AIR2SPV_CONST_DATA_SLOT - 1) + " (ABI.md)");
      off = in->src[1].ssa;
    } else {
      slot = AIR2SPV_CONST_DATA_SLOT; L.usesConstData = true;
      off = nir_iadd_imm(b, in->src[0].ssa, nir_intrinsic_base(in));
    }
    L.maxPushSlot = std::max(L.maxPushSlot, slot);
    unsigned align = nir_intrinsic_align_mul(in);
    if (nir_intrinsic_align_offset(in)) align = 1u << (ffs(nir_intrinsic_align_offset(in)) - 1);
    nir_def *addr = nir_iadd(b, pushAddr(b, slot), nir_u2u64(b, off));
    nir_def *v = nir_build_load_global(b, in->def.num_components, in->def.bit_size, addr,
                                       .align_mul = std::max(align, in->def.bit_size / 8u));
    nir_def_replace(&in->def, v);
    return true;
  }, nir_metadata_control_flow, &L);
}

// nir_to_spirv stores one scalar per store_global.
static bool scalarizeGlobalStores(nir_shader *s) {
  return nir_shader_intrinsics_pass(s, [](nir_builder *b, nir_intrinsic_instr *in, void *) -> bool {
    if (in->intrinsic != nir_intrinsic_store_global || in->src[0].ssa->num_components == 1) return false;
    b->cursor = nir_before_instr(&in->instr);
    nir_def *v = in->src[0].ssa, *a = in->src[1].ssa;
    unsigned wm = nir_intrinsic_write_mask(in), es = v->bit_size / 8;
    for (unsigned c = 0; c < v->num_components; c++)
      if (wm & (1u << c))
        nir_build_store_global(b, nir_channel(b, v, c), nir_iadd_imm(b, a, c * es), .align_mul = es,
                               .access = nir_intrinsic_access(in));
    nir_instr_remove(&in->instr);
    return true;
  }, nir_metadata_control_flow, nullptr);
}

static glsl_base_type baseOfAlu(nir_alu_type t) {
  switch (nir_alu_type_get_base_type(t)) {
  case nir_type_int: return GLSL_TYPE_INT;
  case nir_type_uint: return GLSL_TYPE_UINT;
  default: return GLSL_TYPE_FLOAT;
  }
}

static nir_variable *uniformVar(nir_shader *s, const glsl_type *t, int binding, const char *name) {
  nir_variable *v = nir_variable_create(s, nir_var_uniform, t, name);
  v->data.descriptor_set = 0;
  v->data.binding = binding;
  return v;
}

// tex with texture_index / sampler_index -> texture and separate sampler derefs (set 0).
static bool lowerTextures(nir_shader *s, SpvLayout &L) {
  bool progress = false;
  nir_foreach_function_impl(impl, s) {
    nir_builder b = nir_builder_create(impl);
    nir_foreach_block(blk, impl) nir_foreach_instr_safe(ins, blk) {
      if (ins->type != nir_instr_type_tex) continue;
      nir_tex_instr *t = nir_instr_as_tex(ins);
      if (nir_tex_instr_src_index(t, nir_tex_src_texture_deref) >= 0) continue;
      b.cursor = nir_before_instr(ins);
      int ti = (int)t->texture_index, si = (int)t->sampler_index;
      if (ti < 0 || ti >= 16) die("texture index " + std::to_string(ti));
      glsl_base_type bt = baseOfAlu(t->dest_type);
      if (t->op == nir_texop_txs || t->op == nir_texop_query_levels || t->op == nir_texop_texture_samples) bt = GLSL_TYPE_FLOAT;
      const glsl_type *tt = glsl_texture_type(t->sampler_dim, t->is_array, bt);
      nir_variable *&tv = L.tex[ti];
      if (!tv) tv = uniformVar(s, tt, AIR2SPV_BINDING_TEXTURE + ti, ("texture" + std::to_string(ti)).c_str());
      else if (tv->type != tt) {
        // one Metal texture argument has one type; a size query is typeless
        if (t->op != nir_texop_txs && t->op != nir_texop_query_levels && t->op != nir_texop_texture_samples)
          die("texture(" + std::to_string(ti) + ") used with two image types");
      }
      nir_tex_instr_add_src(t, nir_tex_src_texture_deref, &nir_build_deref_var(&b, tv)->def);
      bool needSampler = t->op == nir_texop_tex || t->op == nir_texop_txb || t->op == nir_texop_txl ||
                         t->op == nir_texop_txd || t->op == nir_texop_lod || t->op == nir_texop_tg4;
      if (needSampler) {
        if (si < 0 || si >= 16) die("sampler index " + std::to_string(si));
        nir_variable *&sv = L.smp[si];
        if (!sv) sv = uniformVar(s, glsl_bare_sampler_type(), AIR2SPV_BINDING_SAMPLER + si, ("sampler" + std::to_string(si)).c_str());
        nir_tex_instr_add_src(t, nir_tex_src_sampler_deref, &nir_build_deref_var(&b, sv)->def);
      }
      t->texture_index = 0; t->sampler_index = 0;
      progress = true;
    }
    nir_progress(progress, impl, nir_metadata_control_flow);
  }
  return progress;
}

// image_* with an index -> image_deref_* on a storage image (set 0, binding 32 + n).
static bool lowerImages(nir_shader *s, SpvLayout &L) {
  return nir_shader_intrinsics_pass(s, [](nir_builder *b, nir_intrinsic_instr *in, void *d) -> bool {
    SpvLayout &L = *(SpvLayout *)d;
    nir_intrinsic_op op;
    switch (in->intrinsic) {
    case nir_intrinsic_image_load: op = nir_intrinsic_image_deref_load; break;
    case nir_intrinsic_image_store: op = nir_intrinsic_image_deref_store; break;
    case nir_intrinsic_image_size: op = nir_intrinsic_image_deref_size; break;
    case nir_intrinsic_image_samples: op = nir_intrinsic_image_deref_samples; break;
    case nir_intrinsic_image_atomic: op = nir_intrinsic_image_deref_atomic; break;
    case nir_intrinsic_image_atomic_swap: op = nir_intrinsic_image_deref_atomic_swap; break;
    default: return false;
    }
    if (!nir_src_is_const(in->src[0])) die("dynamically indexed storage texture");
    int n = (int)nir_src_as_uint(in->src[0]);
    nir_alu_type ty = in->intrinsic == nir_intrinsic_image_store ? nir_intrinsic_src_type(in)
                    : in->intrinsic == nir_intrinsic_image_load ? nir_intrinsic_dest_type(in) : nir_type_float32;
    const glsl_type *it = glsl_image_type(nir_intrinsic_image_dim(in), nir_intrinsic_image_array(in), baseOfAlu(ty));
    nir_variable *&v = L.img[n];
    if (!v) {
      v = uniformVar(b->shader, it, AIR2SPV_BINDING_IMAGE + n, ("image" + std::to_string(n)).c_str());
      v->data.mode = nir_var_image;
      v->data.image.format = PIPE_FORMAT_NONE;
      v->data.access = ACCESS_NON_READABLE;
    }
    if (in->intrinsic != nir_intrinsic_image_store) v->data.access = (gl_access_qualifier)(v->data.access & ~ACCESS_NON_READABLE);
    b->cursor = nir_before_instr(&in->instr);
    nir_intrinsic_instr *n2 = nir_intrinsic_instr_create(b->shader, op);
    n2->num_components = in->num_components;
    n2->src[0] = nir_src_for_ssa(&nir_build_deref_var(b, v)->def);
    for (unsigned i = 1; i < nir_intrinsic_infos[op].num_srcs; i++) n2->src[i] = nir_src_for_ssa(in->src[i].ssa);
    memcpy(n2->const_index, in->const_index, sizeof in->const_index);
    if (nir_intrinsic_infos[op].has_dest) nir_def_init(&n2->instr, &n2->def, in->def.num_components, in->def.bit_size);
    nir_builder_instr_insert(b, &n2->instr);
    if (nir_intrinsic_infos[op].has_dest) nir_def_replace(&in->def, &n2->def);
    else nir_instr_remove(&in->instr);
    return true;
  }, nir_metadata_control_flow, &L);
}

// Framebuffer fetch: a load of a colour output (fb_fetch_output) reads input attachment n
// (set 0, binding 48 + n), as Mesa zink's lower_fbfetch does for one attachment.
static bool lowerFbFetch(nir_shader *s, SpvLayout &L) {
  return nir_shader_intrinsics_pass(s, [](nir_builder *b, nir_intrinsic_instr *in, void *d) -> bool {
    SpvLayout &L = *(SpvLayout *)d;
    if (in->intrinsic != nir_intrinsic_load_deref) return false;
    nir_variable *var = nir_intrinsic_get_var(in, 0);
    if (!var || var->data.mode != nir_var_shader_out || !var->data.fb_fetch_output) return false;
    int rt = var->data.location - FRAG_RESULT_DATA0;
    if (rt < 0 || rt >= 8) die("framebuffer fetch of a non-colour output");
    nir_variable *&fv = L.fb[rt];
    glsl_base_type bt = glsl_get_base_type(glsl_without_array(var->type));
    if (bt != GLSL_TYPE_INT && bt != GLSL_TYPE_UINT) bt = GLSL_TYPE_FLOAT;
    if (!fv) {
      fv = uniformVar(b->shader, glsl_image_type(GLSL_SAMPLER_DIM_SUBPASS, false, bt), AIR2SPV_BINDING_FBFETCH + rt,
                      ("fbfetch" + std::to_string(rt)).c_str());
      fv->data.mode = nir_var_image;
      fv->data.index = rt;     // InputAttachmentIndex
      fv->data.image.format = PIPE_FORMAT_NONE;
    }
    b->cursor = nir_before_instr(&in->instr);
    nir_def *v = nir_image_deref_load(b, 4, 32, &nir_build_deref_var(b, fv)->def, nir_imm_ivec4(b, 0, 0, 0, 0),
                                      nir_undef(b, 1, 32), nir_imm_int(b, 0), .image_dim = GLSL_SAMPLER_DIM_SUBPASS,
                                      .dest_type = (nir_alu_type)(bt == GLSL_TYPE_FLOAT ? nir_type_float32 : bt == GLSL_TYPE_INT ? nir_type_int32 : nir_type_uint32));
    unsigned nc = in->def.num_components, bs = in->def.bit_size;
    v = nir_trim_vector(b, v, nc);
    if (bs != 32) v = bt == GLSL_TYPE_FLOAT ? nir_f2fN(b, v, bs) : nir_u2uN(b, v, bs);
    nir_def_replace(&in->def, v);
    return true;
  }, nir_metadata_control_flow, &L);
}

// SPIR-V Location of user varyings and vertex attributes (ntv reads driver_location).
static void assignLocations(nir_shader *s) {
  nir_foreach_variable_with_modes(v, s, nir_var_shader_in | nir_var_shader_out) {
    int loc = v->data.location;
    if (s->info.stage == MESA_SHADER_VERTEX && v->data.mode == nir_var_shader_in && loc >= VERT_ATTRIB_GENERIC0)
      v->data.driver_location = loc - VERT_ATTRIB_GENERIC0;
    else if (loc >= VARYING_SLOT_VAR0 && !(s->info.stage == MESA_SHADER_FRAGMENT && v->data.mode == nir_var_shader_out))
      v->data.driver_location = loc - VARYING_SLOT_VAR0;
    else
      v->data.driver_location = 0;
  }
}

static void optimize(nir_shader *s) {
  bool p;
  do {
    p = false;
    NIR_PASS(p, s, nir_lower_vars_to_ssa);
    NIR_PASS(p, s, nir_opt_copy_prop);
    NIR_PASS(p, s, nir_opt_dce);
    NIR_PASS(p, s, nir_opt_cse);
    NIR_PASS(p, s, nir_opt_constant_folding);
    NIR_PASS(p, s, nir_opt_algebraic);
    NIR_PASS(p, s, nir_opt_dead_cf);
    NIR_PASS(p, s, nir_opt_if, nir_opt_if_optimize_phi_true_false);
    NIR_PASS(p, s, nir_opt_remove_phis);
    NIR_PASS(p, s, nir_opt_undef);
  } while (p);
}

// nir_to_spirv picks PhysicalStorageBuffer64 addressing only for compute; buffer loads in the
// graphics stages need it too.
static void usePhysicalAddressing(std::vector<uint32_t> &w) {
  bool psb = false;
  for (size_t i = 5; i < w.size();) {
    uint32_t op = w[i] & 0xffff, n = w[i] >> 16;
    if (!n) break;
    if (op == 17 /* OpCapability */ && w[i + 1] == 5347 /* PhysicalStorageBufferAddresses */) psb = true;
    if (op == 14 /* OpMemoryModel */ && psb && w[i + 1] == 0 /* Logical */) w[i + 1] = 5348; /* PhysicalStorageBuffer64 */
    i += n;
  }
}

// ------------------------------------------------------------------------------- compile --
static std::string jstr(const std::string &s) { return "\"" + jesc(s) + "\""; }

struct SpvOut { std::vector<uint32_t> words; std::vector<uint8_t> constData; std::string json, nirText; };

static SpvOut compileSpv(const void *air, size_t size, const AirOptions &o, bool wantNir) {
  AirFrontendResult R = air_frontend_mem(air, size, o, spvNirOptions());
  nir_shader *s = R.nir;
  struct G { nir_shader *s; ~G() { ralloc_free(s); } } guard{s};
  glsl_type_singleton_init_or_ref();
  struct T { ~T() { glsl_type_singleton_decref(); } } tguard;
  if (s->info.stage == MESA_SHADER_COMPUTE) {
    nir_lower_compute_system_values_options csv = {};
    NIR_PASS(_, s, nir_lower_compute_system_values, &csv);
  }
  SpvLayout L;
  NIR_PASS(_, s, lowerBuffers, L);
  NIR_PASS(_, s, lowerTextures, L);
  NIR_PASS(_, s, lowerImages, L);
  if (s->info.stage == MESA_SHADER_FRAGMENT) NIR_PASS(_, s, lowerFbFetch, L);
  if (L.maxPushSlot >= 0) {
    glsl_struct_field f = {};
    f.type = glsl_array_type(glsl_uint_type(), AIR2SPV_PUSH_SLOTS * 2, 4);
    f.name = "addr"; f.offset = 0; f.location = -1;
    nir_variable *pc = nir_variable_create(s, nir_var_mem_push_const, glsl_struct_type(&f, 1, "air2spv_push", false), "push");
    pc->data.location = INT_MAX;
  }
  assignLocations(s);
  NIR_PASS(_, s, nir_lower_int64);
  optimize(s);
  NIR_PASS(_, s, scalarizeGlobalStores);
  NIR_PASS(_, s, nir_opt_algebraic_late);
  NIR_PASS(_, s, nir_opt_copy_prop);
  NIR_PASS(_, s, nir_opt_dce);
  nir_shader_gather_info(s, nir_shader_get_entrypoint(s));
  SpvOut out;
  if (wantNir) { char *t = nir_shader_as_str(s, nullptr); out.nirText = t; ralloc_free(t); }
  NIR_PASS(_, s, nir_convert_from_ssa, true, false);

  ntv_info info = {};
  info.is_native_vulkan = true;
  info.has_demote_to_helper = true;
  info.spirv_version = SPIRV_VERSION(1, 5);
  info.bindless_set_idx = ~0u;
  // Constants are emitted as uint and bitcast where used, so the push-constant member index is an
  // OpConstant even when CSE merged it with a 0.0 float.
  info.broken_arbitary_type_const = true;
  if (s->info.stage == MESA_SHADER_COMPUTE) s->info.cs.ptr_size = 64;
  spirv_shader *sp = nir_to_spirv(s, &info);
  if (!sp) die("nir_to_spirv failed");
  out.words.assign(sp->words, sp->words + sp->num_words);
  spirv_shader_delete(sp);
  usePhysicalAddressing(out.words);
  if (L.usesConstData && R.constant_data_bytes)
    out.constData.assign((const uint8_t *)s->constant_data, (const uint8_t *)s->constant_data + s->constant_data_size);

  // Sidecar
  std::string d = "[";
  auto desc = [&](const char *kind, int metal, int binding, const char *extra = "") {
    if (d.size() > 1) d += ",";
    d += std::string("{\"kind\":\"") + kind + "\",\"index\":" + std::to_string(metal) + ",\"set\":0,\"binding\":" + std::to_string(binding) + extra + "}";
  };
  for (auto &kv : L.tex) desc("sampled_image", kv.first, AIR2SPV_BINDING_TEXTURE + kv.first);
  for (auto &kv : L.smp) desc((R.constexpr_sampler_mask >> kv.first) & 1 ? "constexpr_sampler" : "sampler", kv.first, AIR2SPV_BINDING_SAMPLER + kv.first);
  for (auto &kv : L.img) desc("storage_image", kv.first, AIR2SPV_BINDING_IMAGE + kv.first);
  for (auto &kv : L.fb) desc("input_attachment", kv.first, AIR2SPV_BINDING_FBFETCH + kv.first,
                             (",\"input_attachment_index\":" + std::to_string(kv.first)).c_str());
  d += "]";
  std::string slots = "[";
  for (auto &b : R.bindings)
    if (b.kind == "ubo" && b.used) { if (slots.size() > 1) slots += ","; slots += "{\"slot\":" + std::to_string(b.slot) + ",\"what\":\"constant buffer(" + std::to_string(b.metal_index) + ")\"}"; }
  bool table = false;
  for (auto &b : R.bindings) if ((b.kind == "device_buffer" || b.kind == "threadgroup_buffer") && b.used) table = true;
  if (table) { if (slots.size() > 1) slots += ","; slots += "{\"slot\":" + std::to_string(R.driver_table_ubo) + ",\"what\":\"driver table\"}"; }
  if (L.usesConstData) { if (slots.size() > 1) slots += ","; slots += "{\"slot\":" + std::to_string(AIR2SPV_CONST_DATA_SLOT) + ",\"what\":\"constant data\",\"bytes\":" + std::to_string(out.constData.size()) + "}"; }
  slots += "]";
  out.json = "{\"air2spv_abi\":" + std::to_string(AIR2SPV_ABI_VERSION) + ",\"spirv_words\":" + std::to_string(out.words.size()) +
             ",\"push_constants\":{\"bytes\":" + std::to_string(AIR2SPV_PUSH_BYTES) + ",\"used\":" + (L.maxPushSlot >= 0 ? "true" : "false") +
             ",\"slots\":" + slots + "},\"descriptors\":" + d + ",\"reflection\":" + air_reflection_json(R) +
             ",\"report\":" + jstr(R.report) + "}";
  return out;
}

static char *dupc(const std::string &s) { char *p = (char *)malloc(s.size() + 1); memcpy(p, s.c_str(), s.size() + 1); return p; }

static AirOptions toOptions(const air2spv_options *o) {
  AirOptions a;
  if (!o) return a;
  if (o->entry) a.entry = o->entry;
  a.expectStage = o->stage;
  for (unsigned i = 0; i < o->num_constants; i++) {
    const air2spv_constant &c = o->constants[i];
    std::vector<double> v(c.values, c.values + c.count);
    if (c.index >= 0) a.fc[c.index] = v; else if (c.name) a.fcByName[c.name] = v;
  }
  memcpy(a.rtClass, o->color_format_class, 8);
  memcpy(a.texClass, o->texture_class, 16);
  return a;
}

extern "C" int air2spv_compile(const void *air, size_t size, const air2spv_options *opts, air2spv_result *out) {
  memset(out, 0, sizeof *out);
  try {
    SpvOut r = compileSpv(air, size, toOptions(opts), false);
    out->num_words = r.words.size();
    out->words = (uint32_t *)malloc(r.words.size() * 4);
    memcpy(out->words, r.words.data(), r.words.size() * 4);
    if (!r.constData.empty()) {
      out->constant_data_size = r.constData.size();
      out->constant_data = (uint8_t *)malloc(r.constData.size());
      memcpy(out->constant_data, r.constData.data(), r.constData.size());
    }
    out->json = dupc(r.json);
    return 0;
  } catch (const std::exception &e) {
    out->error = dupc(e.what());
    return 1;
  }
}

extern "C" void air2spv_result_free(air2spv_result *r) {
  if (!r) return;
  free(r->words); free(r->constant_data); free(r->json); free(r->error);
  memset(r, 0, sizeof *r);
}

// ----------------------------------------------------------------------------------- CLI --
#ifndef AIR2SPV_NO_MAIN
static std::vector<double> parseVals(const char *s) {
  std::vector<double> v;
  for (const char *p = s; *p;) { char *e; v.push_back(strtod(p, &e)); if (e == p) break; p = *e == ',' ? e + 1 : e; }
  return v;
}

int main(int argc, char **argv) {
  AirOptions o; const char *in = nullptr, *outPath = nullptr, *jsonPath = nullptr; bool nir = false;
  for (int i = 1; i < argc; i++) {
    std::string a = argv[i];
    auto next = [&]() -> const char * { if (i + 1 >= argc) { fprintf(stderr, "air2spv: %s needs a value\n", a.c_str()); exit(1); } return argv[++i]; };
    if (a == "-o") outPath = next();
    else if (a == "--json") jsonPath = next();
    else if (a == "--entry") o.entry = next();
    else if (a == "--nir") nir = true;
    else if (a == "--stage") { std::string s = next(); o.expectStage = s == "vertex" ? MESA_SHADER_VERTEX : s == "fragment" ? MESA_SHADER_FRAGMENT : MESA_SHADER_COMPUTE; }
    else if (a == "--fc") { std::string s = next(); auto eq = s.find('='); o.fc[atoi(s.substr(0, eq).c_str())] = parseVals(s.c_str() + eq + 1); }
    else if (a == "--fc-name") { std::string s = next(); auto eq = s.find('='); o.fcByName[s.substr(0, eq)] = parseVals(s.c_str() + eq + 1); }
    else if (a == "--rt-class") { std::string s = next(); auto eq = s.find('='); o.rtClass[atoi(s.substr(0, eq).c_str()) & 7] = (uint8_t)atoi(s.c_str() + eq + 1); }
    else if (a == "--tex-class") { std::string s = next(); auto eq = s.find('='); o.texClass[atoi(s.substr(0, eq).c_str()) & 15] = (uint8_t)atoi(s.c_str() + eq + 1); }
    else if (a[0] == '-') {
      fprintf(stderr, "usage: air2spv [--entry name] [--stage vertex|fragment|kernel] [--fc idx=v[,v]] [--fc-name name=v[,v]]\n"
                      "               [--rt-class n=c] [--tex-class n=c] [--nir] [--json out.json] [-o out.spv] function.bc\n");
      return 1;
    } else in = argv[i];
  }
  if (!in) { fprintf(stderr, "air2spv: no input (AIR bitcode of one function, e.g. from tools/metallib_inventory.py --extract)\n"); return 1; }
  auto buf = MemoryBuffer::getFile(in);
  if (!buf) { fprintf(stderr, "air2spv: cannot read %s\n", in); return 1; }
  try {
    SpvOut r = compileSpv((*buf)->getBufferStart(), (*buf)->getBufferSize(), o, nir);
    if (nir) fputs(r.nirText.c_str(), stdout);
    std::string op = outPath ? outPath : std::string(in) + ".spv";
    FILE *f = fopen(op.c_str(), "wb");
    if (!f) { perror(op.c_str()); return 1; }
    fwrite(r.words.data(), 4, r.words.size(), f); fclose(f);
    if (!r.constData.empty()) {
      std::string cp = op + ".const";
      f = fopen(cp.c_str(), "wb"); fwrite(r.constData.data(), 1, r.constData.size(), f); fclose(f);
    }
    if (jsonPath) { f = fopen(jsonPath, "w"); fputs(r.json.c_str(), f); fputc('\n', f); fclose(f); }
    fprintf(stderr, "air2spv: %s -> %s (%zu words)\n", in, op.c_str(), r.words.size());
    return 0;
  } catch (const std::exception &e) {
    fprintf(stderr, "air2spv: UNSUPPORTED/ERROR: %s\n", e.what());
    return 2;
  }
}
#endif
