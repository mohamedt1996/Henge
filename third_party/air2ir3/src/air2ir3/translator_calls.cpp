// SPDX-License-Identifier: MIT
// Translator members defined out of class: texture operations and AIR / LLVM intrinsic calls
// (texop, callAir, callLLVM).
// Part of src/air2ir3.cpp's translation unit, #included from it in order; not compiled on its own.

inline std::vector<nir_def *> Translator::texop(nir_texop op, glsl_sampler_dim dim, bool array, int t, int sm, nir_def *coord,
                                                nir_def *lod, nir_def *bias, nir_def *offset, nir_def *minlod,
                                                nir_def *ddx, nir_def *ddy, unsigned bs, nir_alu_type dt, unsigned ncomp) {
  std::vector<std::pair<nir_tex_src_type, nir_def *>> srcs;
  if (coord) srcs.push_back({nir_tex_src_coord, coord});
  if (lod) srcs.push_back({nir_tex_src_lod, lod});
  if (bias) srcs.push_back({nir_tex_src_bias, bias});
  if (offset) srcs.push_back({nir_tex_src_offset, offset});
  if (minlod) srcs.push_back({nir_tex_src_min_lod, minlod});
  if (ddx) srcs.push_back({nir_tex_src_ddx, ddx});
  if (ddy) srcs.push_back({nir_tex_src_ddy, ddy});
  nir_tex_instr *ti = nir_tex_instr_create(s, srcs.size());
  ti->op = op; ti->sampler_dim = dim; ti->is_array = array; ti->dest_type = dt;
  ti->texture_index = t; ti->sampler_index = sm < 0 ? 0 : sm;
  ti->coord_components = coord ? coord->num_components : 0;
  for (unsigned i = 0; i < srcs.size(); i++) ti->src[i] = nir_tex_src_for_ssa(srcs[i].first, srcs[i].second);
  nir_def_init(&ti->instr, &ti->def, ncomp, bs);
  nir_builder_instr_insert(&b, &ti->instr);
  s->info.num_textures = std::max<unsigned>(s->info.num_textures, t + 1);
  std::vector<nir_def *> r = split(&ti->def);
  unsigned cls = t >= 0 && t < 16 ? opt->texClass[t] : 0;
  if (cls && op != nir_texop_txs && op != nir_texop_query_levels) {
    // Draw-time texture class (BINDING-ABI.md 3.3): the T# is a UNORM view of an XR texture;
    // hardware filtering is linear, so decoding after filtering is exact.
    checkFormatClass(cls, "texture(" + std::to_string(t) + ")");
    if (!(dt & nir_type_float)) die("texture(" + std::to_string(t) + ") has an XR format class but is read as an integer type");
    for (unsigned c = 0; c < r.size(); c++) {
      nir_def *x = r[c]->bit_size == 32 ? r[c] : nir_f2f32(&b, r[c]);
      x = c == 3 && cls == AIR_FMT_XR10_BGR32 ? nir_imm_float(&b, 1.0) : xrDecode(cls, x);
      r[c] = r[c]->bit_size == 32 ? x : nir_f2fN(&b, x, r[c]->bit_size);
    }
    R.tex_class[t] = cls;
  }
  return r;
}

// ---- AIR intrinsic calls ----
inline std::vector<nir_def *> Translator::callAir(CallInst *CI) {
  std::string nm = CI->getCalledFunction()->getName().str();
  auto A = [&](unsigned i) { return get(argOperand(CI, i)); };
  auto A1 = [&](unsigned i) { return get1(argOperand(CI, i)); };
  Type *RT = CI->getType();
  unsigned n = isa<StructType>(RT) || RT->isVoidTy() ? 1 : nflat(RT);
  auto starts = [&](const char *p) { return nm.rfind(p, 0) == 0; };
  auto perComp = [&](auto fn, unsigned nargs) {
    std::vector<std::vector<nir_def *>> in; for (unsigned i = 0; i < nargs; i++) in.push_back(A(i));
    std::vector<nir_def *> r;
    for (unsigned c = 0; c < n; c++) {
      auto pick = [&](unsigned i) { return in[i].size() == 1 ? in[i][0] : in[i][c]; };
      r.push_back(fn(pick));
    }
    return r;
  };
  auto base = [&](const std::string &x) { std::string p = "air." + x + "."; return nm.rfind(p, 0) == 0 || nm == "air." + x; };
#define UN(name, expr) if (base(name) || base("fast_" name)) return perComp([&](auto p) { nir_def *x = p(0); return expr; }, 1);
#define BIN(name, expr) if (base(name) || base("fast_" name)) return perComp([&](auto p) { nir_def *x = p(0), *y = p(1); return expr; }, 2);
  UN("sqrt", nir_fsqrt(&b, x)) UN("rsqrt", nir_frsq(&b, x)) UN("fabs", nir_fabs(&b, x))
  UN("floor", nir_ffloor(&b, x)) UN("ceil", nir_fceil(&b, x)) UN("fract", nir_ffract(&b, x))
  UN("trunc", nir_ftrunc(&b, x)) UN("rint", nir_fround_even(&b, x))
  UN("saturate", nir_fsat(&b, x)) UN("log2", nir_flog2(&b, x)) UN("exp2", nir_fexp2(&b, x))
  UN("log", nir_fmul_imm(&b, nir_flog2(&b, x), 0.69314718055994530942))
  UN("exp", nir_fexp2(&b, nir_fmul_imm(&b, x, 1.44269504088896340736)))
  UN("sin", nir_fsin(&b, x)) UN("cos", nir_fcos(&b, x)) UN("sign", nir_fsign(&b, x))
  // Derivatives of 16-bit values are taken at 32 bits and narrowed: ir3 emits dsx / dsy as f32 (Mesa sets their
  // cat5 type to TYPE_F32), and on the X2-90 the half-register form returns derivatives tens of times too large
  // (A16 v101: Liquid Glass's fwidth(half) lost its shape mask; docs/USERSPACE-GRAPHICS-20261019.md).
  auto wide = [&](nir_def *x) { return x->bit_size == 16 ? nir_f2f32(&b, x) : x; };
  auto narrow = [&](nir_def *x, nir_def *r) { return x->bit_size == 16 ? nir_f2f16(&b, r) : r; };
  UN("dfdx", narrow(x, nir_ddx(&b, wide(x)))) UN("dfdy", narrow(x, nir_ddy(&b, wide(x))))
  UN("fwidth", narrow(x, nir_fadd(&b, nir_fabs(&b, nir_ddx(&b, wide(x))), nir_fabs(&b, nir_ddy(&b, wide(x))))))
  BIN("fmax", nir_fmax(&b, x, y)) BIN("fmin", nir_fmin(&b, x, y))
  BIN("pow", nir_fpow(&b, x, y)) BIN("powr", nir_fpow(&b, x, y)) BIN("atan2", nir_atan2(&b, x, y))
  if (base("fmax3") || base("fast_fmax3")) return perComp([&](auto p) { return nir_fmax(&b, p(0), nir_fmax(&b, p(1), p(2))); }, 3);
  if (base("fmin3") || base("fast_fmin3")) return perComp([&](auto p) { return nir_fmin(&b, p(0), nir_fmin(&b, p(1), p(2))); }, 3);
  // air.clamp.s.* / air.clamp.u.* (integer clamp) also start with "air.clamp.": they must not reach the
  // float clamp.  Fix round 6 (A16 run 2, probe_ubo_lo / probe_ubo_hi): `clamp(int(x) / 6, 0, 15)`
  // compiled to max.f / min.f on the integer's bits; 1..15 are fp32 denormals, which Adreno flushes
  // (Turnip: shaderDenormFlushToZeroFloat32), so every band selected slot 0.
  if (!starts("air.clamp.s.") && !starts("air.clamp.u.") && (base("clamp") || base("fast_clamp")))
    return perComp([&](auto p) { return nir_fmin(&b, nir_fmax(&b, p(0), p(1)), p(2)); }, 3);
  if (base("mix")) return perComp([&](auto p) { return nir_fadd(&b, p(0), nir_fmul(&b, nir_fsub(&b, p(1), p(0)), p(2))); }, 3);
  if (base("fma")) return perComp([&](auto p) { return nir_fadd(&b, nir_fmul(&b, p(0), p(1)), p(2)); }, 3);
  if (starts("air.max.s.")) return perComp([&](auto p) { return nir_imax(&b, p(0), p(1)); }, 2);
  if (starts("air.max.u.")) return perComp([&](auto p) { return nir_umax(&b, p(0), p(1)); }, 2);
  if (starts("air.min.s.")) return perComp([&](auto p) { return nir_imin(&b, p(0), p(1)); }, 2);
  if (starts("air.min.u.")) return perComp([&](auto p) { return nir_umin(&b, p(0), p(1)); }, 2);
  if (starts("air.abs.s.")) return perComp([&](auto p) { return nir_iabs(&b, p(0)); }, 1);
  if (starts("air.clamp.s.")) return perComp([&](auto p) { return nir_imin(&b, nir_imax(&b, p(0), p(1)), p(2)); }, 3);
  if (starts("air.clamp.u.")) return perComp([&](auto p) { return nir_umin(&b, nir_umax(&b, p(0), p(1)), p(2)); }, 3);
  if (base("dot") || base("fast_dot")) return {nir_fdot(&b, vec(A(0)), vec(A(1)))};
  if (base("normalize") || base("fast_normalize")) { nir_def *v = vec(A(0)); return split(nir_fmul(&b, v, nir_frsq(&b, nir_fdot(&b, v, v)))); }
  if (base("length") || base("fast_length")) { nir_def *v = vec(A(0)); return {nir_fsqrt(&b, nir_fdot(&b, v, v))}; }
  if (starts("air.any.")) { auto v = A(0); nir_def *r = v[0]; for (unsigned i = 1; i < v.size(); i++) r = nir_ior(&b, r, v[i]); return {r}; }
  if (starts("air.all.")) { auto v = A(0); nir_def *r = v[0]; for (unsigned i = 1; i < v.size(); i++) r = nir_iand(&b, r, v[i]); return {r}; }
  if (starts("air.normalize_function_constant_predicate")) return {nir_b2iN(&b, nir_ine_imm(&b, A1(0), 0), bits(RT))};
  if (starts("air.unpack.unorm4x8")) return coerce(split(nir_unpack_unorm_4x8(&b, A1(0))), RT);
  if (starts("air.convert.")) {
    // air.convert.<dk>.<dty>.<sk>.<sty>
    SmallVector<StringRef, 6> parts; StringRef(nm).split(parts, '.');
    char dk = parts[2][0], sk = parts[4][0];
    Type *DE = RT->getScalarType(); unsigned db = bits(DE);
    return perComp([&](auto p) -> nir_def * {
      nir_def *x = p(0);
      if (x->bit_size == 1) return dk == 'f' ? nir_b2fN(&b, x, db) : nir_b2iN(&b, x, db);
      if (dk == 'f') return sk == 'f' ? nir_f2fN(&b, x, db) : sk == 's' ? nir_i2fN(&b, x, db) : nir_u2fN(&b, x, db);
      if (db == 1) return sk == 'f' ? nir_fneu_imm(&b, x, 0.0) : nir_ine_imm(&b, x, 0);
      // float -> char / short: through f2i32 (verify6 must-fix 4): Metal truncates toward zero, so
      // char(-128.5) == -128; NIR's f2i8 / f2i16 constant folding calls it poison (source below the
      // minimum before truncation).  No corpus function converts float to a signed 8/16-bit type.
      if (dk == 's' && sk == 'f') return db < 32 ? nir_i2iN(&b, nir_f2i32(&b, x), db) : nir_f2iN(&b, x, db);
      if (dk == 'u' && sk == 'f') return nir_f2uN(&b, x, db);
      // integer -> integer: the SOURCE signedness picks the extension (C semantics; fix round 6)
      return sk == 's' ? nir_i2iN(&b, x, db) : nir_u2uN(&b, x, db);
    }, 1);
  }
  if (starts("air.discard_fragment")) { nir_demote(&b); return {}; }
  if (starts("air.wg.barrier") || starts("air.simdgroup.barrier")) {
    unsigned flags = cast<ConstantInt>(argOperand(CI, 0))->getZExtValue();   // mem_flags: 1 device, 2 threadgroup, 4 texture (INFERRED)
    _nir_barrier_indices ix = {};
    ix.execution_scope = starts("air.wg.") ? SCOPE_WORKGROUP : SCOPE_SUBGROUP; ix.memory_scope = SCOPE_WORKGROUP;
    ix.memory_semantics = NIR_MEMORY_ACQ_REL;
    ix.memory_modes = (nir_variable_mode)((flags & 2 ? nir_var_mem_shared : 0) | (flags & 1 ? nir_var_mem_global : 0) | (flags & 8 ? nir_var_mem_shared | nir_var_image : 0) | (flags & 4 ? nir_var_image : 0));
    _nir_build_barrier(&b, ix); return {};
  }
  if (starts("air.get_simdgroup_size")) return coerce({nir_load_subgroup_size(&b)}, RT);
  if (starts("air.quad_shuffle_xor.") || starts("air.simd_shuffle_xor.")) {
    nir_def *m = nir_u2u32(&b, A1(1)); std::vector<nir_def *> r;
    for (nir_def *x : A(0)) r.push_back(nir_shuffle_xor(&b, x, m)); return r;
  }
  if (starts("air.simd_shuffle.") || starts("air.quad_shuffle.")) {
    nir_def *lane = nir_u2u32(&b, A1(1)); std::vector<nir_def *> r;
    if (starts("air.quad_shuffle.")) lane = nir_ior(&b, nir_iand_imm(&b, nir_load_subgroup_invocation(&b), ~3u), lane);
    for (nir_def *x : A(0)) r.push_back(nir_shuffle(&b, x, lane)); return r;
  }
  if (starts("air.simd_shuffle_and_fill_down.")) {   // (data, filling, delta, modulo)
    nir_def *lane = nir_load_subgroup_invocation(&b), *delta = nir_u2u32(&b, A1(2)), *mod = nir_u2u32(&b, A1(3));
    nir_def *inWin = nir_umod(&b, lane, mod), *src = nir_iadd(&b, lane, delta);
    nir_def *fromData = nir_ult(&b, nir_iadd(&b, inWin, delta), mod);
    nir_def *fillLane = nir_isub(&b, src, mod);
    std::vector<nir_def *> d = A(0), f = A(1), r;
    for (unsigned i = 0; i < d.size(); i++) r.push_back(nir_bcsel(&b, fromData, nir_shuffle(&b, d[i], src), nir_shuffle(&b, f[i], fillLane)));
    return r;
  }
  if (starts("air.simd_sum.") || starts("air.simd_max.") || starts("air.simd_min.")) {
    nir_op op = starts("air.simd_sum.") ? (RT->getScalarType()->isFloatingPointTy() ? nir_op_fadd : nir_op_iadd)
              : starts("air.simd_max.") ? nir_op_fmax : nir_op_fmin;
    std::vector<nir_def *> r; _nir_reduce_indices ix = {}; ix.reduction_op = op; ix.cluster_size = 0;
    for (nir_def *x : A(0)) r.push_back(_nir_build_reduce(&b, x, ix)); return r;
  }
  if (starts("air.write_texture_2d.")) {             // (tex, coord, color, lod, access)
    int t = texOf(argOperand(CI, 0));
    std::vector<nir_def *> c = A(1); for (auto &x : c) x = nir_u2u32(&b, x);
    c.push_back(nir_undef(&b, 1, 32)); c.push_back(nir_undef(&b, 1, 32));
    std::vector<nir_def *> col = A(2);
    _nir_image_store_indices ix = {}; ix.image_dim = GLSL_SAMPLER_DIM_2D; ix.format = PIPE_FORMAT_NONE;
    ix.src_type = (nir_alu_type)(nir_type_float | col[0]->bit_size); ix.access = ACCESS_NON_READABLE;
    _nir_build_image_store(&b, nir_imm_int(&b, t), vec(c), nir_undef(&b, 1, 32), vec(col), nir_u2u32(&b, A1(3)), ix);
    s->info.num_images = std::max<unsigned>(s->info.num_images, t + 1);
    report += "  image store -> image(" + std::to_string(t) + ")\n";
    return {};
  }
  // Textures: (tex, sampler, coord, i1 ?, offset, i1 is_level, float bias|level, float min_lod, i32 0)
  auto resType = [&](unsigned &bs, nir_alu_type &dt) {
    Type *E = cast<StructType>(RT)->getElementType(0)->getScalarType();
    bs = bits(E); dt = (nir_alu_type)((E->isFloatingPointTy() ? nir_type_float : (nm.find(".s.") != std::string::npos ? nir_type_int : nir_type_uint)) | bs);
  };
  auto sampleCommon = [&](glsl_sampler_dim dim, bool arr, unsigned coordArg, int arrArg, unsigned offArg, unsigned lvlFlag) {
    unsigned bs; nir_alu_type dt; resType(bs, dt);
    std::vector<nir_def *> c = A(coordArg);
    if (arr) c.push_back(nir_u2f32(&b, A1(arrArg)));
    nir_def *off = isZeroConst(argOperand(CI, offArg)) ? nullptr : vec(A(offArg));
    bool isLevel = cast<ConstantInt>(argOperand(CI, lvlFlag))->isOne();
    Value *lb = argOperand(CI, lvlFlag + 1), *ml = argOperand(CI, lvlFlag + 2);
    nir_def *lod = isLevel ? A1(lvlFlag + 1) : nullptr;
    nir_def *bias = (!isLevel && !isZeroConst(lb)) ? A1(lvlFlag + 1) : nullptr;
    nir_def *minlod = isZeroConst(ml) ? nullptr : A1(lvlFlag + 2);
    nir_texop op = lod ? nir_texop_txl : bias ? nir_texop_txb : nir_texop_tex;
    if (stage != MESA_SHADER_FRAGMENT && op == nir_texop_tex) { op = nir_texop_txl; lod = nir_imm_float(&b, 0); }
    auto r = texop(op, dim, arr, texOf(argOperand(CI, 0)), smpOf(argOperand(CI, 1)), vec(c), lod, bias, off, minlod, nullptr, nullptr, bs, dt);
    r.push_back(nir_imm_intN_t(&b, 0, 8));           // the i8 "residency" field of the result struct
    return r;
  };
  if (starts("air.sample_texture_2d_grad.")) {
    unsigned bs; nir_alu_type dt; resType(bs, dt);
    auto r = texop(nir_texop_txd, GLSL_SAMPLER_DIM_2D, false, texOf(argOperand(CI, 0)), smpOf(argOperand(CI, 1)),
                   vec(A(2)), nullptr, nullptr, isZeroConst(argOperand(CI, 7)) ? nullptr : vec(A(7)), nullptr, vec(A(3)), vec(A(4)), bs, dt);
    r.push_back(nir_imm_intN_t(&b, 0, 8)); return r;
  }
  if (starts("air.sample_texture_2d.")) return sampleCommon(GLSL_SAMPLER_DIM_2D, false, 2, -1, 4, 5);
  if (starts("air.sample_texture_3d.")) return sampleCommon(GLSL_SAMPLER_DIM_3D, false, 2, -1, 4, 5);
  if (starts("air.sample_texture_1d_array.")) return sampleCommon(GLSL_SAMPLER_DIM_1D, true, 2, 3, 5, 6);
  if (starts("air.sample_texture_1d.")) return sampleCommon(GLSL_SAMPLER_DIM_1D, false, 2, -1, 4, 5);
  if (starts("air.read_texture_2d.")) {
    // Two operand layouts, measured (docs/WINDOWSERVER-APL-ABORT-20261020.md):
    //   AIR 2.9 (system metallibs, `metal -c`):       (tex, read sampler, coord, offset, lod, access)
    //   AIR 2.2 (Metal's compiler service hands the plug-in this version): (tex, coord, lod, access)
    // access is 0 sample, 1 read, 3 read_write; Metal's read() has no offset, so the 2.9 offset is always zero.
    // The layout is told by the operand count and the operand types together; anything else is refused.
    auto intVec2 = [&](unsigned i) {
      auto *V = dyn_cast<FixedVectorType>(argOperand(CI, i)->getType());
      return V && V->getNumElements() == 2 && V->getElementType()->isIntegerTy();
    };
    auto intScalar = [&](unsigned i) { return argOperand(CI, i)->getType()->isIntegerTy(); };
    unsigned coordArg, lodArg;
    if (CI->arg_size() == 6 && argOperand(CI, 1)->getType()->isPointerTy() && intVec2(2) && intVec2(3) && intScalar(4) && intScalar(5)) {
      if (!isZeroConst(argOperand(CI, 3))) die(nm + " with an offset other than a constant zero");
      coordArg = 2; lodArg = 4;
    } else if (CI->arg_size() == 4 && intVec2(1) && intScalar(2) && intScalar(3)) {
      coordArg = 1; lodArg = 2;
    } else die(nm + " with " + std::to_string(CI->arg_size()) + " operands of an unknown layout");
    unsigned bs; nir_alu_type dt; resType(bs, dt);
    nir_def *coord = vec(A(coordArg)); if (coord->bit_size != 32) coord = nir_u2u32(&b, coord);
    nir_def *lod = A1(lodArg); if (lod->bit_size != 32) lod = nir_u2u32(&b, lod);
    auto r = texop(nir_texop_txf, GLSL_SAMPLER_DIM_2D, false, texOf(argOperand(CI, 0)), -1, coord, lod, nullptr, nullptr, nullptr, nullptr, nullptr, bs, dt);
    r.push_back(nir_imm_intN_t(&b, 0, 8)); return r;
  }
  // Multisample size queries have no lod operand; multisample textures are not in the binding ABI.
  if (starts("air.get_width_texture_2d_ms") || starts("air.get_height_texture_2d_ms"))
    die("AIR intrinsic " + nm + ": multisample textures are not supported");
  if (starts("air.get_width_texture_2d") || starts("air.get_height_texture_2d")) {
    auto r = texop(nir_texop_txs, GLSL_SAMPLER_DIM_2D, false, texOf(argOperand(CI, 0)), -1, nullptr, A1(1), nullptr, nullptr, nullptr, nullptr, nullptr, 32, nir_type_uint32, 2);
    return {r[starts("air.get_width") ? 0 : 1]};
  }
  die("AIR intrinsic not implemented in prototype: " + nm);
#undef UN
#undef BIN
}

inline std::vector<nir_def *> Translator::callLLVM(CallInst *CI) {
  Intrinsic::ID id = CI->getIntrinsicID();
  auto A = [&](unsigned i) { return get(argOperand(CI, i)); };
  unsigned n = nflat(CI->getType());
  auto per2 = [&](auto fn) { auto x = A(0), y = A(1); std::vector<nir_def *> r; for (unsigned c = 0; c < n; c++) r.push_back(fn(x[c], y[c])); return r; };
  auto per1 = [&](auto fn) { auto x = A(0); std::vector<nir_def *> r; for (unsigned c = 0; c < n; c++) r.push_back(fn(x[c])); return r; };
  auto per3 = [&](auto fn) { auto x = A(0), y = A(1), z = A(2); std::vector<nir_def *> r; for (unsigned c = 0; c < n; c++) r.push_back(fn(x[c], y[c], z[c])); return r; };
  switch (id) {
  case Intrinsic::lifetime_start: case Intrinsic::lifetime_end: case Intrinsic::assume: return {};
  case Intrinsic::umax: return per2([&](nir_def *x, nir_def *y) { return nir_umax(&b, x, y); });
  case Intrinsic::umin: return per2([&](nir_def *x, nir_def *y) { return nir_umin(&b, x, y); });
  case Intrinsic::smax: return per2([&](nir_def *x, nir_def *y) { return nir_imax(&b, x, y); });
  case Intrinsic::smin: return per2([&](nir_def *x, nir_def *y) { return nir_imin(&b, x, y); });
  case Intrinsic::maxnum: return per2([&](nir_def *x, nir_def *y) { return nir_fmax(&b, x, y); });
  case Intrinsic::minnum: return per2([&](nir_def *x, nir_def *y) { return nir_fmin(&b, x, y); });
  case Intrinsic::fabs: return per1([&](nir_def *x) { return nir_fabs(&b, x); });
  case Intrinsic::floor: return per1([&](nir_def *x) { return nir_ffloor(&b, x); });
  case Intrinsic::sqrt: return per1([&](nir_def *x) { return nir_fsqrt(&b, x); });
  case Intrinsic::fma: case Intrinsic::fmuladd: return per3([&](nir_def *x, nir_def *y, nir_def *z) { return nir_fadd(&b, nir_fmul(&b, x, y), z); });
  case Intrinsic::copysign: return per2([&](nir_def *x, nir_def *y) { return nir_bcsel(&b, nir_flt_imm(&b, y, 0.0), nir_fneg(&b, nir_fabs(&b, x)), nir_fabs(&b, x)); });
  case Intrinsic::ceil: return per1([&](nir_def *x) { return nir_fceil(&b, x); });
  case Intrinsic::trunc: return per1([&](nir_def *x) { return nir_ftrunc(&b, x); });
  case Intrinsic::abs: return per1([&](nir_def *x) { return nir_iabs(&b, x); });
  default: die("LLVM intrinsic not implemented: " + CI->getCalledFunction()->getName().str());
  }
}
