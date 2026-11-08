// SPDX-License-Identifier: MIT
// Translator members defined out of class: render-target lowering of XR and other formats the
// Adreno render backend cannot store (setupRtLowering, xrEncodeOutput).
// Part of src/air2ir3.cpp's translation unit, #included from it in order; not compiled on its own.

// Two ways to blend into a lowered (XR) attachment (BINDING-ABI.md 4.5, FORMATS.md 3):
//  - "hw": the RB blends in the view's u-domain; the shader writes an offset-corrected source.
//    Only for blend states that are exact for EVERY source value: complementary factor pairs,
//    a zero source factor, a zero destination factor, min and max; never with source colour
//    (2, 3) as an RGB factor, which the RB would clamp to [0, 1] (ABI v5 fix round 5, verify5 item 1).
//  - "shader" (ABI v3): the shader reads the destination by framebuffer fetch, decodes it, blends
//    exactly as Metal does (source clamped to the XR range, then s*Fs op d*Fd) and stores the
//    exact code; the RB's blending is off.  Used for premultiplied source-over, whose hw form is
//    exact only for premultiplied in-range sources (|s| <= range*a; verify2 item 7: tint -0.7 with
//    partial alpha was off by 26 codes), for additive / subtract / destination factors (refused in
//    ABI v3), and whenever the hw form would need dual-source blending while another attachment
//    is written (Adreno dual-source drives colour(0) only; verify2 item 7, work/mrt.json).
inline void Translator::setupRtLowering(int idx, nir_variable *v, bool written) {
  unsigned cls = opt->rtClass[idx];
  checkFormatClass(cls, "colour attachment " + std::to_string(idx));
  if (glsl_get_base_type(v->type) != GLSL_TYPE_FLOAT) die("integer [[color(" + std::to_string(idx) + ")]] on an XR attachment");
  v->type = glsl_vec4_type();
  RtLow L; L.cls = cls; L.api = opt->blend[idx]; L.hw = L.api; L.var = v;
  if (written && L.api.enabled) {
    const AirBlend &a = L.api;
    bool alphaStored = cls == AIR_FMT_XR10_BGRA64;
    // min / max ignore the blend factors (Metal, like Vulkan and GL): the factors of such a channel
    // are neither checked nor used, and the hw blend state carries ONE/ONE there (verify3 item 2:
    // v3 handed the app's factors to the RB, remapped SRC_* to SRC1_* and even turned dual-source
    // on, which forced shader blending whenever another attachment was written).
    bool rgbMM = a.rgbOp == 3 || a.rgbOp == 4, alphaMM = a.alphaOp == 3 || a.alphaOp == 4;
    unsigned fs[4] = {a.srcRGB, a.dstRGB, a.srcA, a.dstA};
    for (int i = 0; i < 4; i++) {
      if (!alphaStored && i >= 2) continue;                 // 554: alpha is not stored (write-masked)
      if (i < 2 ? rgbMM : alphaMM) continue;               // min / max: factors ignored
      if (fs[i] >= 11) die("blend factor " + std::to_string(fs[i]) + " (blend colour or source1) on an XR attachment: not in ABI v5; conversion-blit path");
    }
    if (a.rgbOp > 4 || (alphaStored && a.alphaOp > 4)) die("blend operation " + std::to_string(a.rgbOp) + " on an XR attachment");
    // hw form exact for every source?  (u_s = k*s + c*(1 - Fd) must stay in [0,1] for all s in the XR range)
    auto hwExact = [&](unsigned op, unsigned sf, unsigned df, bool rgb) {
      if (op == 3 || op == 4) return true;                  // min / max ignore the factors
      if (op != 0) return false;                            // subtract / reverse subtract
      if (sf > 5 || df > 5) return false;                   // destination factors
      // Source colour as a factor on RGB (2, 3): the RB reads it from dual-source output 1 of the
      // UNORM view and clamps it to [0, 1]; Metal uses it clamped only to the XR range
      // [-0.753, 1.251].  Not exact outside [0, 1] (verify5 item 1: 76..439 codes off, fixtures
      // p000..p035, r554_204): the shader path.  On the alpha channel these factors read the source
      // alpha, which is in [0, 1] anyway (verify4 item 3), so they stay on the RB.
      if (rgb && (sf == 2 || sf == 3 || df == 2 || df == 3)) return false;
      bool compl_ = (sf == 4 && df == 5) || (sf == 5 && df == 4) || (sf == 2 && df == 3) || (sf == 3 && df == 2);
      return compl_ || sf == 0 || (sf == 1 && df == 0);   // u_s = k*s + c*(1 - Fd) assumes Fs = 1 (or complementary)
    };
    bool hw = hwExact(a.rgbOp, a.srcRGB, a.dstRGB, true) && (!alphaStored || hwExact(a.alphaOp, a.srcA, a.dstA, false));
    bool needsDual = false;
    if (hw) {
      // Factors that read the source colour see the encoded value: route the raw colour through
      // dual-source output 1 and use the SRC1 factors (MTLBlendFactorSource1* = 15..18).
      // A min / max channel has no factors, so it never needs dual-source.
      bool rgbDual = !rgbMM && (srcFactor(a.srcRGB) || srcFactor(a.dstRGB));
      needsDual = rgbDual;
      if (alphaStored) needsDual = needsDual || (!alphaMM && (srcFactor(a.srcA) || srcFactor(a.dstA)));
      else needsDual = !rgbMM && (a.srcRGB == 2 || a.srcRGB == 3 || a.dstRGB == 2 || a.dstRGB == 3);   // alpha output stays raw on 554
      if (needsDual && (idx != 0 || writtenRts > 1)) hw = false;   // Adreno dual-source: colour(0) only, no other attachment
    }
    if (hw) {
      if (rgbMM) { L.hw.srcRGB = 1; L.hw.dstRGB = 1; }       // min / max: ONE/ONE (the factors are ignored)
      if (alphaMM) { L.hw.srcA = 1; L.hw.dstA = 1; }
      if (a.srcRGB == 0 && a.rgbOp == 0) L.hw.srcRGB = 1;   // ZERO source factor: the shader outputs only the offset term
      if (alphaStored && a.srcA == 0 && a.alphaOp == 0) L.hw.srcA = 1;
      if (needsDual) {
        L.dual = true;
        auto remap = [](uint8_t &f) { if (f >= 2 && f <= 5) f = (uint8_t)(f + 13); };
        remap(L.hw.srcRGB); remap(L.hw.dstRGB); remap(L.hw.srcA); remap(L.hw.dstA);
        L.src1 = nir_variable_create(s, nir_var_shader_out, glsl_vec4_type(), "xr_dual_src1");
        L.src1->data.location = FRAG_RESULT_DATA0; L.src1->data.index = 1;
        s->info.fs.color_is_dual_source = true;
      }
    } else {
      L.fetch = true;
      L.hw = AirBlend();                                    // RB blending off: the shader stores the blended code
      v->data.fb_fetch_output = 1;
      s->info.fs.uses_fbfetch_output = true;
      R.uses_fb_fetch = true;
      bool have = false;
      for (auto &bd : R.bindings) if (bd.kind == "fb_fetch" && bd.metal_index == idx) have = true;
      if (!have) {
        AirBinding bd; bd.kind = "fb_fetch"; bd.metal_index = idx; bd.slot = ABI_FB_READ_SLOT + idx;
        bd.arg_name = "(xr blend: destination)"; R.bindings.push_back(bd);
      }
    }
    if (L.dual && writtenRts > 1) die("internal: dual-source XR lowering with another attachment written");
  }
  L.ri = idx;
  rtLow[idx] = L;
  AirRtLowering rl; rl.index = idx; rl.cls = cls; rl.api = L.api; rl.hw = L.hw; rl.dual_source = L.dual; rl.blend_in_shader = L.fetch;
  rl.clear_alpha_write = cls == AIR_FMT_XR10_BGR32;
  for (auto &x : R.rt_lowering) if (x.index == idx) { x = rl; return; }
  R.rt_lowering.push_back(rl);
}

// The four channels of a lowered colour output, in the view's u-domain (BINDING-ABI.md 4.5).
inline nir_def *Translator::xrEncodeOutput(RtLow &L, std::vector<nir_def *> sv) {
  while (sv.size() < 3) sv.push_back(nir_imm_float(&b, 0));
  if (sv.size() < 4) sv.push_back(nir_imm_float(&b, 1));
  for (auto &x : sv) if (x->bit_size != 32) x = nir_f2f32(&b, x);
  double k = xrK(L.cls), c = xrC(L.cls);
  const AirBlend &a = L.api;
  // Metal clamps the source to the format's representable range before blending (as Vulkan does
  // for normalized formats): [-384/510, 638/510] = [-0.752941, 1.25098], the range Apple documents
  // for the XR10 formats (codes 0..1022).  Measured: without it premultiplied-over differed by up to
  // 21 codes where a bright source met a dark destination; with the upper bound at 639/510 (ABI v3)
  // verify3's subtract scene v3xr554_40 (s*s - d*d, source red up to 1.47) was 3 codes off on 122
  // pixels, and 638/510 brings it to <= 1 code (tools/xrrand.py, scratch/xr40/model.py).
  for (unsigned ch = 0; ch < 4; ch++)
    if (a.enabled && (ch < 3 || L.cls == AIR_FMT_XR10_BGRA64))
      sv[ch] = nir_fmin(&b, nir_fmax(&b, sv[ch], nir_imm_float(&b, -384.0 / 510.0)), nir_imm_float(&b, 638.0 / 510.0));
  // ... and the source ALPHA to [0, 1] wherever it is a blend factor (source alpha, one minus source
  // alpha, source alpha saturate, and source colour on the alpha channel), on both XR formats
  // (verify4 item 3: with alpha 1.2, premultiplied source-over was 117 codes off and subtract with
  // source alpha 104 / 224, because 1 - 1.2 blended in a negative share of the destination).
  nir_def *sa = a.enabled ? nir_fsat(&b, sv[3]) : sv[3];
  if (L.fetch) {
    // Blend in the shader, exactly as Metal: read the destination (framebuffer fetch of the view),
    // decode, s*Fs op d*Fd per channel, then store the exact code (RB blending off).
    std::vector<nir_def *> u = split(nir_load_var(&b, L.var)), d;
    for (unsigned ch = 0; ch < 4; ch++)
      d.push_back(ch == 3 && L.cls == AIR_FMT_XR10_BGR32 ? nir_imm_float(&b, 1.0) : xrDecode(L.cls, u[ch]));
    auto factor = [&](unsigned f, unsigned ch, bool alpha) -> nir_def * {
      switch (f) {
      case 0: return nir_imm_float(&b, 0); case 1: return nir_imm_float(&b, 1);
      case 2: return ch == 3 ? sa : sv[ch]; case 3: return nir_fsub_imm(&b, 1.0, ch == 3 ? sa : sv[ch]);
      case 4: return sa; case 5: return nir_fsub_imm(&b, 1.0, sa);
      case 6: return d[ch]; case 7: return nir_fsub_imm(&b, 1.0, d[ch]);
      case 8: return d[3]; case 9: return nir_fsub_imm(&b, 1.0, d[3]);
      case 10: return alpha ? nir_imm_float(&b, 1.0) : nir_fmin(&b, sa, nir_fsub_imm(&b, 1.0, d[3]));
      }
      die("blend factor");
    };
    // BGRA10_XR alpha is not extended range: its source value is clamped to [0, 1] too before
    // blending (measured: a552_sub_sa_12, alpha 1.2 with subtract, is off by 102 codes otherwise).
    auto blend = [&](unsigned ch, bool alpha) -> nir_def * {
      unsigned op = alpha ? a.alphaOp : a.rgbOp, sf = alpha ? a.srcA : a.srcRGB, df = alpha ? a.dstA : a.dstRGB;
      nir_def *s_ = ch == 3 ? sa : sv[ch];
      if (op == 3) return nir_fmin(&b, s_, d[ch]);
      if (op == 4) return nir_fmax(&b, s_, d[ch]);
      nir_def *x = nir_fmul(&b, s_, factor(sf, ch, alpha)), *y = nir_fmul(&b, d[ch], factor(df, ch, alpha));
      return op == 0 ? nir_fadd(&b, x, y) : op == 1 ? nir_fsub(&b, x, y) : nir_fsub(&b, y, x);
    };
    // BGRA10_XR alpha is not extended range: Metal clamps the blended alpha to [0, 1] (measured:
    // scene xr552_tint_additive, alpha 1 + d stored as code 894 = 1.0).
    nir_def *o[4] = {xrEncodeExact(L.cls, blend(0, false)), xrEncodeExact(L.cls, blend(1, false)), xrEncodeExact(L.cls, blend(2, false)),
                     L.cls == AIR_FMT_XR10_BGR32 ? nir_imm_float(&b, 1.0) : xrEncodeExact(L.cls, nir_fsat(&b, blend(3, true)))};
    return nir_vec(&b, o, 4);
  }
  // RB path: the shader hands the RB a source alpha already in [0, 1] (the factor Metal uses; on
  // BGRA10_XR also the stored alpha, which Metal clamps to [0, 1] after blending: for every blend
  // state the hw path takes, blending the clamped alpha gives the same result), so the result does
  // not depend on whether the RB clamps SRC_ALPHA / SRC1_ALPHA for the UNORM view (verify4 item 3:
  // a552_srcover_12, alpha 1.2, was 102 codes off on llvmpipe).
  sv[3] = sa;
  auto factor = [&](unsigned f, unsigned ch) -> nir_def * {
    switch (f) {
    case 0: return nir_imm_float(&b, 0); case 1: return nir_imm_float(&b, 1);
    case 2: return sv[ch]; case 3: return nir_fsub_imm(&b, 1.0, sv[ch]);
    case 4: return sv[3]; case 5: return nir_fsub_imm(&b, 1.0, sv[3]);
    }
    die("blend factor");
  };
  auto chan = [&](unsigned ch, bool alpha) -> nir_def * {
    if (!a.enabled) return xrEncodeExact(L.cls, sv[ch]);
    unsigned op = alpha ? a.alphaOp : a.rgbOp, sf = alpha ? a.srcA : a.srcRGB, df = alpha ? a.dstA : a.dstRGB;
    nir_def *ks = nir_fadd_imm(&b, nir_fmul_imm(&b, sv[ch], k), 0.0);
    if (op != 0) return nir_fadd_imm(&b, ks, c);                                  // min / max
    if (sf == 1) return nir_fadd(&b, ks, nir_fmul_imm(&b, nir_fsub_imm(&b, 1.0, factor(df, ch)), c));
    if (sf == 0) return nir_fmul_imm(&b, nir_fsub_imm(&b, 1.0, factor(df, ch)), c);
    return nir_fadd_imm(&b, ks, c);                                               // complementary pair
  };
  nir_def *o[4] = {chan(0, false), chan(1, false), chan(2, false),
                   L.cls == AIR_FMT_XR10_BGR32 ? sv[3] : chan(3, true)};
  if (L.dual) nir_store_var(&b, L.src1, nir_vec4(&b, sv[0], sv[1], sv[2], sv[3]), 0xf);
  return nir_vec(&b, o, 4);
}
