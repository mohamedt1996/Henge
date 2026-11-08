// SPDX-License-Identifier: MIT
// Front-end entry points: entry lookup, function-constant resolution, air_frontend_mem, reflection JSON.
// Part of src/air2ir3.cpp's translation unit, #included from it in order; not compiled on its own.

// ---------------------------------------------------------------------- front end --
const char *air_format_class_name(unsigned c) {
  static const char *n[] = {"native", "xr10_bgr32", "xr10_bgra64", "xr10_rgb10a8_2p", "ycbcr10_packed", "xr10_srgb"};
  return c < AIR_FMT_CLASS_COUNT ? n[c] : "invalid";
}

AirFrontendResult air_frontend(const char *path, const AirOptions &o, const nir_shader_compiler_options *nopts) {
  auto buf = MemoryBuffer::getFile(path);
  if (!buf) die(std::string("cannot read input ") + path);
  return air_frontend_mem((*buf)->getBufferStart(), (*buf)->getBufferSize(), o, nopts);
}

// Entry point via !air.vertex / !air.fragment / !air.kernel.  A metallib function module has
// exactly one entry; a .air from `metal -c` may have several (select with AirOptions::entry / --entry).
struct AirEntry { Function *F = nullptr; mesa_shader_stage stage = MESA_SHADER_NONE; const MDNode *ins = nullptr, *outs = nullptr; };
static AirEntry findEntry(Module *M, const AirOptions &o) {
  AirEntry e;
  for (auto [md, st] : {std::pair{"air.vertex", MESA_SHADER_VERTEX}, {"air.fragment", MESA_SHADER_FRAGMENT}, {"air.kernel", MESA_SHADER_COMPUTE}}) {
    if (NamedMDNode *N = M->getNamedMetadata(md)) {
      for (MDNode *E : N->operands()) {
        Function *C = cast<Function>(cast<ValueAsMetadata>(E->getOperand(0))->getValue());
        if (!o.entry.empty() ? C->getName() != o.entry : (e.F != nullptr || (o.expectStage >= 0 && (int)st != o.expectStage))) continue;
        e.F = C; e.stage = st;
        if (st == MESA_SHADER_COMPUTE) { e.ins = cast<MDNode>(E->getOperand(2)); e.outs = nullptr; }
        else { e.outs = cast<MDNode>(E->getOperand(1)); e.ins = cast<MDNode>(E->getOperand(2)); }
      }
    }
  }
  if (!e.F && !o.entry.empty()) die("entry point " + o.entry + " not found");
  if (!e.F) {
    if (M->getNamedMetadata("air.visible")) die("no AIR entry point (visible/stitchable function: needs linking into a caller)");
    die("no AIR entry point");
  }
  if (o.expectStage >= 0 && o.expectStage != (int)e.stage)
    die("entry point " + e.F->getName().str() + " is a " + (e.stage == MESA_SHADER_VERTEX ? "vertex" : e.stage == MESA_SHADER_FRAGMENT ? "fragment" : "kernel") +
        " function, not the requested stage");
  return e;
}
// Function constants the module declares, with the values the caller set (by index or by name).
static std::vector<AirFc> resolveFcs(const std::vector<FcDecl> &decls, const AirOptions &o, const std::string &fn,
                                     std::map<int, std::vector<double>> &fc) {
  fc = o.fc;
  for (auto &kv : o.fcByName) {
    bool found = false;
    for (auto &d : decls) if (d.name == kv.first) { fc[d.index] = kv.second; found = true; }
    if (!found) fprintf(stderr, "air2ir3: warning: function constant '%s' not declared by %s (ignored)\n", kv.first.c_str(), fn.c_str());
  }
  std::vector<AirFc> r;
  for (auto &d : decls) {
    AirFc f; f.index = d.index; f.type = d.type; f.name = d.name; f.defined = fc.count(d.index) != 0;
    if (f.defined) fc[d.index] = fcCanon(d.type, fc.at(d.index));     // the value in the declared type (format 5)
    f.value = f.defined ? fc.at(d.index) : std::vector<double>{};
    r.push_back(f);
  }
  return r;
}
// texture(n) arguments the function DECLARES (gated or not): the draw-time texture classes of the
// other T# slots are not key material and do not reach the translator (CACHE-FORMAT.md 2.4).
static uint16_t declaredTextures(const std::vector<ArgInfo> &inArgs) {
  uint16_t m = 0;
  for (auto &a : inArgs) if (a.kind == "air.texture" && a.location >= 0 && a.location < 16) m |= 1u << a.location;
  return m;
}
static int declaredUboMax(const std::vector<ArgInfo> &inArgs) {
  int m = -1;
  for (auto &a : inArgs) if (a.kind == "air.buffer" && a.addrspace == 2 && a.location >= 0 && a.location <= 30) m = std::max(m, a.location);
  return m;
}

// Mesa's NIR_PASS validates the whole shader after every pass in builds without NDEBUG (runtime v5's Mesa is
// debugoptimized): a debugging aid that was 27% of a generic CoreAnimation ubershader compile. Off, as in Mesa's release
// builds, unless NIR_DEBUG is set (left as the developer set it) or AIR2IR3_NIR_VALIDATE=1 (the regression tests).
// Set once, before the first shader is created; nir_debug is only read afterwards.
static void nirValidationPolicy() {
  static std::once_flag once;
  std::call_once(once, [] {
    nir_process_debug_variable();
    const char *v = getenv("AIR2IR3_NIR_VALIDATE");
    if (!getenv("NIR_DEBUG") && !(v && atoi(v))) nir_debug |= NIR_DEBUG_NOVALIDATE;
  });
}

AirFrontendResult air_frontend_mem(const void *bitcode, size_t size, const AirOptions &o0, const nir_shader_compiler_options *nopts) {
  nirValidationPolicy();
  LLVMContext Ctx;                        // one context per compile: no state shared between calls
  AirFrontendResult R;
  MemoryBufferRef mref(StringRef((const char *)bitcode, size), "air");
  auto modOr = parseBitcodeFile(mref, Ctx);
  if (!modOr) die("bitcode parse failed: " + toString(modOr.takeError()));
  std::unique_ptr<Module> MOwn = std::move(*modOr);
  Module *M = MOwn.get();

  AirEntry E = findEntry(M, o0);
  Function *F = E.F; mesa_shader_stage stage = E.stage; const MDNode *ins = E.ins, *outsMd = E.outs;
  R.function = F->getName().str(); R.stage = stage; R.triple = M->getTargetTriple().str();

  // Metadata first (the specialiser may delete the globals it references).
  std::vector<ArgInfo> inArgs, outArgs;
  for (unsigned i = 0; outsMd && i < outsMd->getNumOperands(); i++) outArgs.push_back(parseArg(cast<MDNode>(outsMd->getOperand(i)), true));
  for (unsigned i = 0; ins && i < ins->getNumOperands(); i++) inArgs.push_back(parseArg(cast<MDNode>(ins->getOperand(i)), false));
  for (auto &a : inArgs) {
    if (a.argno < 0 || (unsigned)a.argno >= F->arg_size()) die("input argument index out of range");
    if (a.kind == "air.buffer") {
      auto *PT = dyn_cast<PointerType>(F->getArg(a.argno)->getType());
      if (!PT) die("buffer argument is not a pointer");
      int actual = (int)PT->getAddressSpace();
      if (a.addrspace >= 0 && a.addrspace != actual) die("buffer address-space metadata disagrees with pointer type");
      a.addrspace = actual;
    }
    if (a.kind == "air.imageblock")
      die("input kind air.imageblock (imageblocks/tile memory are Apple-family-4+ features; not in ABI v5, COVERAGE.md 6.1)");
    if (a.kind == "air.visible_function_table" || a.kind == "air.indirect_buffer") die("input kind " + a.kind);
  }
  R.declared_textures = declaredTextures(inArgs);
  R.declared_ubo_max = declaredUboMax(inArgs);
  if (R.declared_ubo_max > uboSlots(o0) - 1)
    die("constant buffer(" + std::to_string(R.declared_ubo_max) + "): a8xx honours only UBO 0.." + std::to_string(uboSlots(o0) - 1) +
        " and constant buffer n is UBO n (ABI v6, BINDING-ABI.md 3.1)");
  R.driver_table_ubo = R.declared_ubo_max + 1;
  AirOptions o = o0;
  for (int i = 0; i < 16; i++) if (!(R.declared_textures & (1u << i))) o.texClass[i] = 0;

  // Function constants: by index and by name.
  std::vector<FcDecl> decls = parseFcDecls(*M);
  std::map<int, std::vector<double>> fc;
  R.fcs = resolveFcs(decls, o, R.function, fc);
  SpecResult spec = specialize_and_inline(*M, F, decls, fc);
  if (getenv("AIR2IR3_DUMP_LL")) { std::error_code ec; raw_fd_ostream os(getenv("AIR2IR3_DUMP_LL"), ec); M->print(os, nullptr); }

  glsl_type_singleton_init_or_ref();
  std::unique_ptr<Translator> T(new Translator(*M, F, stage, R, spec));
  T->inArgs = inArgs; T->outArgs = outArgs; T->opt = &o;
  for (int i = 0; i < 16; i++) if (o.texClass[i]) Translator::checkFormatClass(o.texClass[i], "texture(" + std::to_string(i) + ")");
  try { R.nir = T->run(nopts); }
  catch (...) { if (T->s) ralloc_free(T->s); glsl_type_singleton_decref(); throw; }
  glsl_type_singleton_decref();
  // discard_fragment -> demote
  nir_foreach_function_impl(impl, R.nir) nir_foreach_block(blk, impl) nir_foreach_instr(ins2, blk)
    if (ins2->type == nir_instr_type_intrinsic && nir_instr_as_intrinsic(ins2)->intrinsic == nir_intrinsic_demote) R.uses_discard = true;
  std::string impl;
  if (M->getGlobalVariable("llvm.global_ctors") || !spec.implicitVals.empty())
    impl += std::string("  static initialiser evaluated: ") + (spec.ctorEvaluated ? "yes" : "no") + "\n";
  for (auto &kv : spec.implicitVals) impl += "  implicit " + kv.first + " = " + std::to_string(kv.second) + "\n";
  if (spec.taintedPredicates) impl += "  unset function constants: " + std::to_string(spec.taintedPredicates) + " predicate(s) forced false (Metal semantics)\n";
  if (spec.unsetMarkers)
    impl += "  unset function constants in the body: " + std::to_string(spec.unsetMarkers) + " load(s), " + std::to_string(spec.unsetIndependent) +
            " use(s) independent of the value (folded); nothing decided by them\n";
  R.report = impl + T->report;
  return R;
}

static std::string fcValueJson(const AirFc &f);
std::string air_reflection_json(const AirFrontendResult &r) {
  std::string j = "{";
  auto kv = [&](const std::string &k, const std::string &v, bool q = true) { if (j.size() > 1) j += ","; j += "\"" + k + "\":" + (q ? "\"" + jesc(v) + "\"" : v); };
  kv("abi_version", std::to_string(ABI_VERSION), false);
  kv("function", r.function);
  kv("stage", r.stage == MESA_SHADER_VERTEX ? "vertex" : r.stage == MESA_SHADER_FRAGMENT ? "fragment" : "kernel");
  kv("air_triple", r.triple);
  std::string a = "[";
  for (auto &f : r.fcs) {
    if (a.size() > 1) a += ",";
    a += "{\"index\":" + std::to_string(f.index) + ",\"name\":\"" + jesc(f.name) + "\",\"type\":\"" + jesc(f.type) +
         "\",\"defined\":" + (f.defined ? "true" : "false") + "," + fcValueJson(f) + "}";
  }
  kv("function_constants", a + "]", false);
  a = "[";
  for (auto &b : r.bindings) {
    if (a.size() > 1) a += ",";
    a += "{\"kind\":\"" + b.kind + "\",\"metal_index\":" + std::to_string(b.metal_index) + ",\"slot\":" + std::to_string(b.slot);
    if (!b.arg_name.empty()) a += ",\"arg\":\"" + jesc(b.arg_name) + "\"";
    if (!b.type_name.empty()) a += ",\"type\":\"" + jesc(b.type_name) + "\"";
    if (!b.access.empty()) a += ",\"access\":\"" + b.access + "\"";
    if (b.kind == "ubo" && b.minimum_bytes >= 0) a += ",\"minimum_bytes\":" + std::to_string(b.minimum_bytes);
    a += std::string(",\"present\":") + (b.present ? "true" : "false");
    a += std::string(",\"used\":") + (b.used ? "true" : "false");
    if (b.kind == "fb_fetch") a += ",\"bindless_set\":" + std::to_string(ABI_FS_BINDLESS_SET);
    if (b.kind == "device_buffer" || b.kind == "threadgroup_buffer") a += ",\"driver_table_ubo\":" + std::to_string(r.driver_table_ubo);
    if (b.kind == "constexpr_sampler") {
      char buf[512]; air_sampler as; air_sampler_decode(b.sampler_word, &as); char dec[640]; air_sampler_json(&as, dec, sizeof dec);
      snprintf(buf, sizeof buf, ",\"air_word\":\"0x%016llx\",\"unique_id\":\"0x%016llx\",\"a8xx_tex_samp\":[\"0x%08x\",\"0x%08x\",\"0x%08x\",\"0x%08x\"],\"state\":",
               (unsigned long long)b.sampler_word, (unsigned long long)b.unique_id, b.a8xx_desc[0], b.a8xx_desc[1], b.a8xx_desc[2], b.a8xx_desc[3]);
      a += buf; a += dec;
    }
    a += "}";
  }
  kv("bindings", a + "]", false);
  char m[32]; snprintf(m, sizeof m, "0x%04x", r.constexpr_sampler_mask); kv("constexpr_sampler_mask", m);
  a = "[";
  for (auto &v : r.varyings) {
    if (a.size() > 1) a += ",";
    a += "{\"name\":\"" + jesc(v.name) + "\",\"slot\":" + std::to_string(v.slot) + ",\"components\":" + std::to_string(v.comps) +
         ",\"bits\":" + std::to_string(v.bits) + ",\"type\":\"" + v.base + "\",\"interp\":\"" + v.interp + "\",\"sampling\":\"" + v.sampling + "\"}";
  }
  kv("varyings", a + "]", false);
  a = "[";
  for (auto &v : r.attributes) {
    if (a.size() > 1) a += ",";
    a += "{\"attribute\":" + std::to_string(v.index) + ",\"components\":" + std::to_string(v.comps) + ",\"bits\":" + std::to_string(v.bits) + ",\"type\":\"" + v.base + "\"}";
  }
  kv("vertex_attributes", a + "]", false);
  a = "[";
  for (auto &v : r.render_targets) {
    if (a.size() > 1) a += ",";
    a += "{\"index\":" + std::to_string(v.index) + ",\"components\":" + std::to_string(v.comps) + ",\"type\":\"" + v.base +
         "\",\"written\":" + (v.written ? "true" : "false") + ",\"fb_fetch\":" + (v.fb_fetch ? "true" : "false") + "}";
  }
  kv("render_targets", a + "]", false);
  auto blendJ = [](const AirBlend &x) {
    char bb[200]; snprintf(bb, sizeof bb, "{\"enabled\":%s,\"rgb_op\":%u,\"alpha_op\":%u,\"src_rgb\":%u,\"dst_rgb\":%u,\"src_alpha\":%u,\"dst_alpha\":%u}",
                           x.enabled ? "true" : "false", x.rgbOp, x.alphaOp, x.srcRGB, x.dstRGB, x.srcA, x.dstA);
    return std::string(bb);
  };
  a = "[";
  for (auto &l : r.rt_lowering) {
    if (a.size() > 1) a += ",";
    a += "{\"index\":" + std::to_string(l.index) + ",\"class\":" + std::to_string(l.cls) + ",\"class_name\":\"" + air_format_class_name(l.cls) +
         "\",\"view_format\":\"" + (l.cls == AIR_FMT_XR10_BGR32 ? "B10G10R10A2_UNORM" : "R16G16B16A16_UNORM, components stored B,G,R,A") +
         "\",\"api_blend\":" + blendJ(l.api) + ",\"hw_blend\":" + blendJ(l.hw) + ",\"dual_source\":" + (l.dual_source ? "true" : "false") +
         ",\"blend_in_shader\":" + (l.blend_in_shader ? "true" : "false") +
         ",\"hw_write_mask_clear\":\"" + (l.clear_alpha_write ? "A" : "") + "\"}";
  }
  kv("rt_lowering", a + "]", false);
  a = "{";
  for (int t = 0; t < 16; t++) if (r.tex_class[t]) { if (a.size() > 1) a += ","; a += "\"" + std::to_string(t) + "\":" + std::to_string(r.tex_class[t]); }
  kv("texture_classes", a + "}", false);
  a = "["; for (auto &s : r.sysvals) { if (a.size() > 1) a += ","; a += "\"" + s + "\""; } kv("system_values", a + "]", false);
  kv("writes_depth", r.writes_depth ? "true" : "false", false);
  kv("writes_point_size", r.writes_point_size ? "true" : "false", false);
  kv("uses_discard", r.uses_discard ? "true" : "false", false);
  kv("uses_fb_fetch", r.uses_fb_fetch ? "true" : "false", false);
  kv("constant_data_bytes", std::to_string(r.constant_data_bytes), false);
  kv("threadgroup_bytes", std::to_string(r.shared_bytes), false);
  a = "["; for (int x : r.requires_no_attachment) { if (a.size() > 1) a += ","; a += std::to_string(x); } kv("requires_no_attachment", a + "]", false);
  a = "["; for (auto &w : r.warnings) { if (a.size() > 1) a += ","; a += "\"" + jesc(w) + "\""; } kv("warnings", a + "]", false);
  return j + "}";
}
