// SPDX-License-Identifier: MIT
// The AIR -> NIR translator: PtrVal and struct Translator (data members, member declarations and
// the small members defined in class; the larger members follow in translator_*.cpp).
// Part of src/air2ir3.cpp's translation unit, #included from it in order; not compiled on its own.

// ------------------------------------------------------------- translator ---
struct PtrVal {
  enum Kind { UBO, GLOBAL, SHARED, SAMPLER_CONST, CONSTDATA, PRIVATE } kind;
  nir_variable *var = nullptr;     // PRIVATE: uint8 array
  int index = -1;                 // UBO index
  nir_def *addr64 = nullptr;      // GLOBAL base
  nir_def *off = nullptr;         // 32-bit byte offset
  const GlobalVariable *gv = nullptr;
};

struct Translator {
  Module &M; const DataLayout &DL; Function *F; mesa_shader_stage stage;
  AirFrontendResult &R; const SpecResult &spec;
  std::vector<ArgInfo> inArgs, outArgs;
  nir_shader *s = nullptr; nir_builder b;
  std::unordered_map<const Value *, std::vector<nir_def *>> vals;
  std::unordered_map<const Value *, PtrVal> ptrs;
  std::unordered_map<const Value *, int> tex, smp;
  std::unordered_map<const Value *, nir_def *> tgBase;          // threadgroup(n) argument -> base byte offset
  const AirOptions *opt = nullptr;
  // XR colour attachments lowered in the shader (FORMATS.md, BINDING-ABI.md 4.5)
  struct RtLow { unsigned cls = 0; AirBlend api, hw; bool dual = false, fetch = false; nir_variable *src1 = nullptr, *var = nullptr; int ri = -1; };
  std::map<int, RtLow> rtLow;                                  // by render-target index

  // ---- XR10 affine encoding: u = k*v + c in the UNORM view of the attachment/texture ----
  // 554 BGR10_XR: 10-bit fields, v = (n - 384) / 510, viewed as B10G10R10A2_UNORM (u = n / 1023)
  // 552 BGRA10_XR: 10 bits in the top of 16-bit words, viewed as R16G16B16A16_UNORM (u = 64 n / 65535)
  static double xrK(unsigned cls) { return cls == AIR_FMT_XR10_BGR32 ? 510.0 / 1023.0 : 510.0 * 64.0 / 65535.0; }
  static double xrC(unsigned cls) { return cls == AIR_FMT_XR10_BGR32 ? 384.0 / 1023.0 : 384.0 * 64.0 / 65535.0; }
  nir_def *xrDecode(unsigned cls, nir_def *u) { return nir_fmul_imm(&b, nir_fadd_imm(&b, u, -xrC(cls)), 1.0 / xrK(cls)); }
  // Exact encoding (no blending): round to the 10-bit code first so the UNORM store is bit-exact.
  nir_def *xrEncodeExact(unsigned cls, nir_def *v) {
    nir_def *n = nir_fround_even(&b, nir_fmin(&b, nir_fmax(&b, nir_fadd_imm(&b, nir_fmul_imm(&b, v, 510.0), 384.0), nir_imm_float(&b, 0)), nir_imm_float(&b, 1023)));
    return nir_fmul_imm(&b, n, cls == AIR_FMT_XR10_BGR32 ? 1.0 / 1023.0 : 64.0 / 65535.0);
  }
  static void checkFormatClass(unsigned cls, const std::string &what) {
    if (cls == AIR_FMT_NATIVE || cls == AIR_FMT_XR10_BGR32 || cls == AIR_FMT_XR10_BGRA64) return;
    if (cls == AIR_FMT_XR10_RGB10A8_2P || cls == AIR_FMT_XR10_SRGB)
      die(what + " has format class " + air_format_class_name(cls) + ": no shader lowering in ABI v5; the command stream uses the conversion-blit path (FORMATS.md)");
    if (cls == AIR_FMT_YCBCR10_PACKED)
      die(what + " has format class " + air_format_class_name(cls) + ": sampled through a conversion blit, never rendered (FORMATS.md)");
    die(what + ": unknown format class " + std::to_string(cls));
  }
  static bool srcFactor(unsigned f) { return f >= 2 && f <= 5; }
  int writtenRts = 0;                                          // render targets the function writes (dual-source check)
  void setupRtLowering(int idx, nir_variable *v, bool written);
  nir_def *xrEncodeOutput(RtLow &L, std::vector<nir_def *> sv);
  std::map<const GlobalVariable *, int> constSamplers;       // constexpr sampler -> S# (assigned up front)
  std::map<const BasicBlock *, nir_block *> blocks;
  std::map<const PHINode *, nir_variable *> phiVars;
  // Pointer phis (docs/USERSPACE-GRAPHICS-20261019.md, RenderBox's path and alpha-effect shaders): a 32-bit offset from
  // one base (root, or the single threadgroup / constant-data space when root is null), or a 64-bit device address.
  struct PtrPhi { nir_variable *var = nullptr; PtrVal::Kind kind = PtrVal::GLOBAL; Value *root = nullptr; bool addr64 = false; };
  std::map<const PHINode *, PtrPhi> ptrPhis;
  std::vector<std::pair<nir_variable *, unsigned>> outs;     // per return scalar: var (nullptr = inactive), comp
  std::map<std::string, int> varyingSlot;                    // name -> VAR slot (sorted-name order)
  int maxUbo = -1;
  // ABI v6: the driver table's UBO index; a function whose declared constant buffers leave no room
  // for it below 16 is refused (no WindowServer function comes close: they declare buffer(0..7)).
  int driverTableUbo() {
    int maxIdx = uboSlots(*opt) - 1;
    if (R.driver_table_ubo > maxIdx)
      die("device or threadgroup buffer with constant buffers declared up to buffer(" + std::to_string(R.declared_ubo_max) +
          "): the driver table would be UBO " + std::to_string(R.driver_table_ubo) + ", but a8xx honours only UBO 0.." +
          std::to_string(maxIdx) + " (ABI v6, BINDING-ABI.md 3.2)");
    return R.driver_table_ubo;
  }
  std::map<const GlobalVariable *, unsigned> sharedOff; unsigned sharedSize = 0;
  std::map<const GlobalVariable *, unsigned> constOff; std::vector<uint8_t> constData;
  std::map<const GlobalVariable *, nir_variable *> privVars;
  uint64_t allocaBytes = 0;
  std::string report;

  Translator(Module &m, Function *f, mesa_shader_stage st, AirFrontendResult &r, const SpecResult &sp)
      : M(m), DL(m.getDataLayout()), F(f), stage(st), R(r), spec(sp) {}

  // ---- types ----
  static void flatten(Type *T, std::vector<Type *> &o) {
    if (auto *V = dyn_cast<FixedVectorType>(T)) { for (unsigned i = 0; i < V->getNumElements(); i++) o.push_back(V->getElementType()); }
    else if (auto *S = dyn_cast<StructType>(T)) { for (Type *E : S->elements()) flatten(E, o); }
    else if (auto *A = dyn_cast<ArrayType>(T)) { for (unsigned i = 0; i < A->getNumElements(); i++) flatten(A->getElementType(), o); }
    else o.push_back(T);
  }
  static unsigned nflat(Type *T) { std::vector<Type *> v; flatten(T, v); return v.size(); }
  static unsigned bits(Type *T) {
    if (T->isIntegerTy()) return T->getIntegerBitWidth();
    if (T->isHalfTy()) return 16; if (T->isFloatTy()) return 32; if (T->isDoubleTy()) return 64;
    if (T->isPointerTy()) return 64;
    die("unsupported scalar type");
  }
  // GLSL base type for a scalar LLVM type (bool, 8/16/32/64-bit int, half/float/double).
  // Prototype bug fixed: 64-bit values (i64 phis in prefix-sum kernels, 64-bit addresses) were
  // declared as 32-bit uint temporaries ("bit size mismatch comp 0 for phi i64").
  static glsl_base_type baseOf(Type *E) {
    unsigned bs = bits(E);
    if (bs == 1) return GLSL_TYPE_BOOL;
    if (E->isFloatingPointTy()) return bs == 16 ? GLSL_TYPE_FLOAT16 : bs == 64 ? GLSL_TYPE_DOUBLE : GLSL_TYPE_FLOAT;
    switch (bs) {
    case 8: return GLSL_TYPE_UINT8; case 16: return GLSL_TYPE_UINT16; case 32: return GLSL_TYPE_UINT;
    case 64: return GLSL_TYPE_UINT64;
    default: die("unsupported integer width " + std::to_string(bs));
    }
  }
  // ---- values ----
  std::vector<nir_def *> cst(Constant *C) {
    std::vector<nir_def *> r;
    Type *T = C->getType();
    if (isa<UndefValue>(C) || isa<PoisonValue>(C) || isa<ConstantAggregateZero>(C) || isa<ConstantPointerNull>(C)) {
      std::vector<Type *> ts; flatten(T, ts);
      for (Type *e : ts) r.push_back(isa<ConstantAggregateZero>(C) || isa<ConstantPointerNull>(C)
                                       ? nir_imm_zero(&b, 1, bits(e)) : nir_undef(&b, 1, bits(e)));
      return r;
    }
    if (auto *CF = dyn_cast<ConstantFP>(C)) { r.push_back(nir_imm_floatN_t(&b, CF->getValueAPF().convertToDouble(), bits(T))); return r; }
    if (auto *CI = dyn_cast<ConstantInt>(C)) {
      if (bits(T) == 1) r.push_back(nir_imm_bool(&b, CI->isOne()));
      else r.push_back(nir_imm_intN_t(&b, CI->getSExtValue(), bits(T)));
      return r;
    }
    if (auto *CDS = dyn_cast<ConstantDataSequential>(C)) {
      for (unsigned i = 0; i < CDS->getNumElements(); i++) { auto v = cst(CDS->getElementAsConstant(i)); r.insert(r.end(), v.begin(), v.end()); }
      return r;
    }
    if (isa<ConstantVector>(C) || isa<ConstantStruct>(C) || isa<ConstantArray>(C)) {
      for (Use &U : C->operands()) { auto v = cst(cast<Constant>(U.get())); r.insert(r.end(), v.begin(), v.end()); }
      return r;
    }
    die("unsupported constant: " + str(C));
  }
  std::vector<nir_def *> get(Value *V) {
    auto it = vals.find(V);
    if (it != vals.end()) return it->second;
    if (auto *C = dyn_cast<Constant>(V)) if (!isa<GlobalValue>(C)) return cst(C);
    die("value not translated: " + str(V));
  }
  nir_def *get1(Value *V) { auto v = get(V); if (v.size() != 1) die("expected scalar: " + str(V)); return v[0]; }
  nir_def *vec(const std::vector<nir_def *> &c) { return nir_vec(&b, const_cast<nir_def **>(c.data()), c.size()); }
  std::vector<nir_def *> split(nir_def *d) { std::vector<nir_def *> r; for (unsigned i = 0; i < d->num_components; i++) r.push_back(nir_channel(&b, d, i)); return r; }

  // Serialise a constant initialiser to bytes (little endian, DataLayout offsets).
  void bytesOf(Constant *C, std::vector<uint8_t> &o, size_t at) {
    Type *T = C->getType();
    if (isa<ConstantAggregateZero>(C) || isa<UndefValue>(C) || isa<ConstantPointerNull>(C)) return;
    if (auto *CI = dyn_cast<ConstantInt>(C)) { APInt v = CI->getValue(); unsigned n = (T->getIntegerBitWidth() + 7) / 8;
      for (unsigned i = 0; i < n; i++) o[at + i] = (uint8_t)v.extractBitsAsZExtValue(std::min(8u, T->getIntegerBitWidth() - i * 8), i * 8); return; }
    if (auto *CF = dyn_cast<ConstantFP>(C)) { APInt v = CF->getValueAPF().bitcastToAPInt(); unsigned n = v.getBitWidth() / 8;
      for (unsigned i = 0; i < n; i++) o[at + i] = (uint8_t)v.extractBitsAsZExtValue(8, i * 8); return; }
    if (auto *CDS = dyn_cast<ConstantDataSequential>(C)) { StringRef raw = CDS->getRawDataValues();
      uint64_t es = DL.getTypeAllocSize(CDS->getElementType());
      for (unsigned i = 0; i < CDS->getNumElements(); i++) memcpy(&o[at + i * es], raw.data() + i * CDS->getElementByteSize(), CDS->getElementByteSize());
      return; }
    if (auto *CS = dyn_cast<ConstantStruct>(C)) { const StructLayout *SL = DL.getStructLayout(CS->getType());
      for (unsigned i = 0; i < CS->getNumOperands(); i++) bytesOf(CS->getOperand(i), o, at + SL->getElementOffset(i)); return; }
    if (isa<ConstantArray>(C) || isa<ConstantVector>(C)) { Type *E = isa<ArrayType>(T) ? T->getArrayElementType() : cast<VectorType>(T)->getElementType();
      uint64_t es = DL.getTypeAllocSize(E);
      for (unsigned i = 0; i < C->getNumOperands(); i++) bytesOf(cast<Constant>(C->getOperand(i)), o, at + i * es); return; }
    die("cannot serialise initialiser: " + str(C));
  }

  // ---- pointers ----
  // The objects a pointer can point into, through GEPs, casts, phis and selects; nullptr stands for any other origin
  // (a loaded, integer-made or returned pointer). undef/poison incoming values point nowhere.
  void ptrRoots(Value *V, std::set<Value *> &roots, std::set<Value *> &seen) {
    if (!seen.insert(V).second || isa<UndefValue>(V)) return;
    if (auto *GEP = dyn_cast<GEPOperator>(V)) return ptrRoots(GEP->getPointerOperand(), roots, seen);
    if (auto *O = dyn_cast<Operator>(V))
      if (O->getOpcode() == Instruction::BitCast || O->getOpcode() == Instruction::AddrSpaceCast) return ptrRoots(O->getOperand(0), roots, seen);
    if (auto *PN = dyn_cast<PHINode>(V)) { for (Value *in : PN->incoming_values()) ptrRoots(in, roots, seen); return; }
    if (auto *S = dyn_cast<SelectInst>(V)) { ptrRoots(S->getTrueValue(), roots, seen); ptrRoots(S->getFalseValue(), roots, seen); return; }
    roots.insert(isa<Argument>(V) || isa<GlobalVariable>(V) || isa<AllocaInst>(V) ? V : nullptr);
  }
  // The PtrVal kind a root resolves to, without emitting code (as setupArgs() and ptr() decide); -1 if unsupported.
  int rootKind(Value *R) {
    if (auto *A = dyn_cast<Argument>(R)) { auto it = ptrs.find(A); return it == ptrs.end() ? -1 : it->second.kind; }
    if (auto *GV = dyn_cast<GlobalVariable>(R)) {
      if (GV->getName().starts_with("__air_sampler_state")) return -1;
      if (GV->getAddressSpace() == 3) return PtrVal::SHARED;
      if (GV->hasInitializer() && GV->isConstant()) return PtrVal::CONSTDATA;
      if (GV->hasInitializer() && GV->hasLocalLinkage()) return PtrVal::PRIVATE;
      return -1;
    }
    return isa<AllocaInst>(R) ? PtrVal::PRIVATE : -1;
  }
  // Two pointers address the same object space, so their offsets can be phi'd or selected.
  static bool sameBase(const PtrVal &a, const PtrVal &c) {
    if (a.kind != c.kind) return false;
    switch (a.kind) {
    case PtrVal::SHARED: case PtrVal::CONSTDATA: return true;      // one space each: offsets are absolute
    case PtrVal::UBO: return a.index == c.index;
    case PtrVal::GLOBAL: return a.addr64 && a.addr64 == c.addr64;
    case PtrVal::PRIVATE: return a.var && a.var == c.var;
    default: return false;
    }
  }
  nir_def *globalAddress(const PtrVal &p) { return nir_iadd(&b, p.addr64, nir_u2u64(&b, p.off)); }
  PtrPhi ptrPhiOf(PHINode *PN) {
    std::set<Value *> roots, seen; ptrRoots(PN, roots, seen);
    if (roots.count(nullptr)) die("phi of pointers that are loaded, made from integers or returned by calls: " + str(PN));
    if (roots.empty()) die("phi of undefined pointers: " + str(PN));
    std::set<int> kinds; for (Value *R : roots) kinds.insert(rootKind(R));
    if (kinds.count(-1)) die("phi of unsupported pointers (sampler state or unknown arguments): " + str(PN));
    PtrPhi pp; pp.kind = (PtrVal::Kind)*kinds.begin();
    if (roots.size() == 1) pp.root = *roots.begin();
    else if (kinds.size() == 1 && (pp.kind == PtrVal::SHARED || pp.kind == PtrVal::CONSTDATA)) pp.root = nullptr;
    else if (kinds.size() == 1 && pp.kind == PtrVal::GLOBAL) pp.addr64 = true;
    else die("phi of pointers into different buffers or address spaces (" + std::to_string(roots.size()) + " objects): " + str(PN));
    pp.var = nir_local_variable_create(b.impl, pp.addr64 ? glsl_uint64_t_type() : glsl_uint_type(), "pphi");
    return pp;
  }
  PtrVal ptrPhiBase(const PtrPhi &pp) { if (pp.root) return ptr(pp.root); PtrVal p{pp.kind}; return p; }
  PtrVal ptrPhiValue(const PtrPhi &pp) {
    nir_def *l = nir_load_var(&b, pp.var);
    if (pp.addr64) { PtrVal p{PtrVal::GLOBAL}; p.addr64 = l; p.off = nir_imm_int(&b, 0); return p; }
    PtrVal p = ptrPhiBase(pp); p.off = l; return p;
  }
  void storePtrPhi(const PtrPhi &pp, Value *in, PHINode *PN) {
    if (isa<UndefValue>(in)) { nir_store_var(&b, pp.var, pp.addr64 ? nir_imm_int64(&b, 0) : nir_imm_int(&b, 0), 1); return; }
    PtrVal p = ptr(in);
    if (pp.addr64) {
      if (p.kind != PtrVal::GLOBAL || !p.addr64) die("pointer phi incoming value is not a device-buffer pointer: " + str(PN));
      nir_store_var(&b, pp.var, globalAddress(p), 1); return;
    }
    if (!sameBase(p, ptrPhiBase(pp))) die("pointer phi incoming value has another base than the phi: " + str(PN));
    nir_store_var(&b, pp.var, p.off, 1);
  }

  PtrVal ptr(Value *V) {
    auto it = ptrs.find(V);
    if (it != ptrs.end()) return it->second;
    if (auto *GV = dyn_cast<GlobalVariable>(V)) {
      if (GV->getName().starts_with("__air_sampler_state")) { PtrVal p{PtrVal::SAMPLER_CONST}; p.gv = GV; return p; }
      if (GV->getAddressSpace() == 3) {             // threadgroup (static) -> shared memory
        auto it2 = sharedOff.find(GV);
        if (it2 == sharedOff.end()) {
          unsigned al = std::max<unsigned>(16, GV->getAlign() ? GV->getAlign()->value() : 16);
          sharedSize = (sharedSize + al - 1) & ~(al - 1);
          it2 = sharedOff.emplace(GV, sharedSize).first;
          sharedSize += DL.getTypeAllocSize(GV->getValueType());
        }
        PtrVal p{PtrVal::SHARED}; p.off = nir_imm_int(&b, it2->second); return p;
      }
      if (GV->hasInitializer() && GV->isConstant()) {      // constant table -> shader constant data (ir3 const UBO)
        auto it2 = constOff.find(GV);
        if (it2 == constOff.end()) {
          size_t at = (constData.size() + 15) & ~size_t(15); uint64_t sz = DL.getTypeAllocSize(GV->getValueType());
          constData.resize(at + sz, 0); bytesOf(GV->getInitializer(), constData, at);
          it2 = constOff.emplace(GV, at).first;
          report += "  constant table " + GV->getName().str() + " (" + std::to_string(sz) + " bytes) -> constant data @" + std::to_string(at) + "\n";
        }
        PtrVal p{PtrVal::CONSTDATA}; p.off = nir_imm_int(&b, it2->second); return p;
      }
      if (GV->hasInitializer() && GV->hasLocalLinkage()) {  // mutable per-invocation state (fc shadows, statics)
        auto it2 = privVars.find(GV);
        if (it2 == privVars.end()) {
          uint64_t sz = DL.getTypeAllocSize(GV->getValueType());
          nir_variable *v = nir_local_variable_create(b.impl, glsl_array_type(glsl_uint8_t_type(), sz, 1), GV->getName().str().c_str());
          std::vector<uint8_t> init(sz, 0); bytesOf(GV->getInitializer(), init, 0);
          nir_builder ib = nir_builder_at(nir_before_impl(b.impl));
          for (uint64_t i = 0; i < sz; i++)
            nir_store_deref(&ib, nir_build_deref_array_imm(&ib, nir_build_deref_var(&ib, v), i), nir_imm_intN_t(&ib, init[i], 8), 1);
          it2 = privVars.emplace(GV, v).first;
        }
        PtrVal p{PtrVal::PRIVATE}; p.var = it2->second; p.off = nir_imm_int(&b, 0); return p;
      }
      die("global variable access not supported: " + GV->getName().str());
    }
    if (auto *GEP = dyn_cast<GEPOperator>(V)) {
      PtrVal p = ptr(GEP->getPointerOperand());
      unsigned bw = DL.getIndexTypeSizeInBits(GEP->getType());
      APInt constOff(bw, 0);
      SmallMapVector<Value *, APInt, 4> varOffs;
      if (!GEP->collectOffset(DL, bw, varOffs, constOff)) die("GEP offset: " + str(V));
      nir_def *o = nir_iadd_imm(&b, p.off, (int64_t)constOff.getSExtValue());
      for (auto &kv : varOffs) {
        nir_def *iv = get1(kv.first);
        iv = iv->bit_size == 32 ? iv : nir_i2i32(&b, iv);
        o = nir_iadd(&b, o, nir_imul_imm(&b, iv, kv.second.getSExtValue()));
      }
      p.off = o; ptrs[V] = p; return p;
    }
    if (auto *C = dyn_cast<ConstantExpr>(V)) if (C->getOpcode() == Instruction::AddrSpaceCast || C->getOpcode() == Instruction::BitCast) return ptr(C->getOperand(0));
    die("unsupported pointer: " + str(V));
  }
  void loadScalars(const PtrVal &p, Type *T, int64_t extra, unsigned align, std::vector<nir_def *> &out) {
    if (auto *S = dyn_cast<StructType>(T)) {
      const StructLayout *SL = DL.getStructLayout(S);
      for (unsigned i = 0; i < S->getNumElements(); i++) loadScalars(p, S->getElementType(i), extra + SL->getElementOffset(i), 4, out);
      return;
    }
    if (auto *A = dyn_cast<ArrayType>(T)) {
      uint64_t es = DL.getTypeAllocSize(A->getElementType());
      for (unsigned i = 0; i < A->getNumElements(); i++) loadScalars(p, A->getElementType(), extra + i * es, 4, out);
      return;
    }
    Type *E = T->getScalarType();
    unsigned n = isa<FixedVectorType>(T) ? cast<FixedVectorType>(T)->getNumElements() : 1;
    unsigned bs = bits(E);
    nir_def *off = nir_iadd_imm(&b, p.off, extra);
    if (bs < 16 && (p.kind == PtrVal::PRIVATE)) {
      for (unsigned i = 0; i < n; i++) {
        nir_def *byte = load(p, nir_iadd_imm(&b, off, i), 1, 8, 1);
        out.push_back(bs == 1 ? nir_ine_imm(&b, byte, 0) : byte);
      }
      return;
    }
    if (bs < 16) {                                    // bool / i8: load the containing dword, extract bytes
      for (unsigned i = 0; i < n; i++) {
        nir_def *o = nir_iadd_imm(&b, off, i);
        nir_def *dw = load(p, nir_iand_imm(&b, o, ~3u), 1, 32, 4);
        nir_def *byte = nir_ubitfield_extract(&b, dw, nir_imul_imm(&b, nir_iand_imm(&b, o, 3), 8), nir_imm_int(&b, 8));
        out.push_back(bs == 1 ? nir_ine_imm(&b, byte, 0) : nir_u2uN(&b, byte, bs));
      }
      return;
    }
    nir_def *v = load(p, off, n, bs, std::max(align, bs / 8));
    for (unsigned i = 0; i < n; i++) out.push_back(nir_channel(&b, v, i));
  }
  nir_def *load(const PtrVal &p, nir_def *off, unsigned n, unsigned bs, unsigned align) {
    if (p.kind == PtrVal::UBO) {
      _nir_load_ubo_indices ix = {}; ix.align_mul = align; ix.align_offset = 0; ix.range_base = 0; ix.range = ~0u;
      return _nir_build_load_ubo(&b, n, bs, nir_imm_int(&b, p.index), off, ix);
    }
    if (p.kind == PtrVal::GLOBAL) {
      _nir_load_global_indices ix = {}; ix.align_mul = align; ix.align_offset = 0;
      return _nir_build_load_global(&b, n, bs, nir_iadd(&b, p.addr64, nir_u2u64(&b, off)), ix);
    }
    if (p.kind == PtrVal::CONSTDATA) {
      _nir_load_constant_indices ix = {}; ix.base = 0; ix.range = ~0u; ix.align_mul = align;
      return _nir_build_load_constant(&b, n, bs, off, ix);
    }
    if (p.kind == PtrVal::PRIVATE) {                 // byte-granular private storage
      std::vector<nir_def *> by;
      for (unsigned i = 0; i < n * bs / 8; i++)
        by.push_back(nir_load_deref(&b, nir_build_deref_array(&b, nir_build_deref_var(&b, p.var), nir_iadd_imm(&b, off, i))));
      return nir_extract_bits(&b, by.data(), by.size(), 0, n, bs);
    }
    if (p.kind == PtrVal::SHARED) {
      _nir_load_shared_indices ix = {}; ix.align_mul = align; ix.align_offset = 0;
      return _nir_build_load_shared(&b, n, bs, off, ix);
    }
    die("load from unsupported pointer kind");
  }

  void store(const PtrVal &p, nir_def *off, nir_def *v, unsigned align) {
    if (p.kind == PtrVal::GLOBAL) {
      _nir_store_global_indices ix = {}; ix.write_mask = (1u << v->num_components) - 1; ix.align_mul = align;
      _nir_build_store_global(&b, v, nir_iadd(&b, p.addr64, nir_u2u64(&b, off)), ix); return;
    }
    if (p.kind == PtrVal::SHARED) {
      _nir_store_shared_indices ix = {}; ix.write_mask = (1u << v->num_components) - 1; ix.align_mul = align;
      _nir_build_store_shared(&b, v, off, ix); return;
    }
    if (p.kind == PtrVal::PRIVATE) {
      nir_def *by = nir_extract_bits(&b, &v, 1, 0, v->num_components * v->bit_size / 8, 8);
      for (unsigned i = 0; i < by->num_components; i++)
        nir_store_deref(&b, nir_build_deref_array(&b, nir_build_deref_var(&b, p.var), nir_iadd_imm(&b, off, i)), nir_channel(&b, by, i), 1);
      return;
    }
    die("store to read-only (constant address space) pointer");
  }
  void storeScalars(const PtrVal &p, Type *T, const std::vector<nir_def *> &v, unsigned &idx, int64_t extra, unsigned align) {
    if (auto *S = dyn_cast<StructType>(T)) { const StructLayout *SL = DL.getStructLayout(S);
      for (unsigned i = 0; i < S->getNumElements(); i++) storeScalars(p, S->getElementType(i), v, idx, extra + SL->getElementOffset(i), 4); return; }
    if (auto *A = dyn_cast<ArrayType>(T)) { uint64_t es = DL.getTypeAllocSize(A->getElementType());
      for (unsigned i = 0; i < A->getNumElements(); i++) storeScalars(p, A->getElementType(), v, idx, extra + i * es, 4); return; }
    unsigned n = isa<FixedVectorType>(T) ? cast<FixedVectorType>(T)->getNumElements() : 1;
    std::vector<nir_def *> c(v.begin() + idx, v.begin() + idx + n); idx += n;
    for (auto &d : c) if (d->bit_size == 1) d = nir_b2i8(&b, d);
    store(p, nir_iadd_imm(&b, p.off, extra), vec(c), std::max<unsigned>(align, c[0]->bit_size / 8));
  }
  // Convert loaded/system values to the bit sizes of the LLVM argument type.
  std::vector<nir_def *> coerce(std::vector<nir_def *> v, Type *T) {
    std::vector<Type *> ts; flatten(T, ts);
    if (v.size() > ts.size()) v.resize(ts.size());
    for (unsigned i = 0; i < v.size(); i++) {
      unsigned bs = bits(ts[i]);
      if (v[i]->bit_size == bs) continue;
      v[i] = ts[i]->isFloatingPointTy() ? nir_f2fN(&b, v[i], bs) : nir_u2uN(&b, v[i], bs);
    }
    return v;
  }


  // ---- IO ----
  static std::string baseName(glsl_base_type bt) {
    switch (bt) {
    case GLSL_TYPE_FLOAT: return "float"; case GLSL_TYPE_FLOAT16: return "half"; case GLSL_TYPE_UINT: return "uint";
    case GLSL_TYPE_INT: return "int"; case GLSL_TYPE_UINT16: return "ushort"; case GLSL_TYPE_BOOL: return "bool";
    default: return "other";
    }
  }
  nir_variable *mkvar(nir_variable_mode mode, Type *T, int loc, const char *nm, bool force32 = false) {
    std::vector<Type *> ts; flatten(T, ts);
    Type *E = ts[0];
    unsigned eb = force32 ? 32 : bits(E);
    glsl_base_type bt = E->isFloatingPointTy() ? (eb == 16 ? GLSL_TYPE_FLOAT16 : GLSL_TYPE_FLOAT)
                                               : (eb == 16 ? GLSL_TYPE_UINT16 : GLSL_TYPE_UINT);
    nir_variable *v = nir_variable_create(s, mode, glsl_vector_type(bt, ts.size()), nm);
    v->data.location = loc;
    return v;
  }
  // Evaluate a gating predicate: an implicit predicate global evaluated from the static initialiser,
  // or a function-constant initialiser global directly.  -1: unknown (gatedPresent refuses it).
  int predValue(const std::string &g) {
    auto am = spec.ambiguousPredicates.find(g);
    if (am != spec.ambiguousPredicates.end()) die(am->second);
    auto it = spec.implicitVals.find(g);
    if (it != spec.implicitVals.end()) return it->second != 0;
    int idx = fcIndexOfGlobal(g);
    if (idx >= 0) {
      for (auto &f : R.fcs) if (f.index == idx) return !f.value.empty() && f.value[0] != 0;
      return 0;
    }
    return -1;   // unknown
  }
  // fix round 5 (verify5 item 2): a gating predicate the static-initialiser evaluation could not
  // decide refuses the variant.  v4 fell back to "present if the code reads it" for arguments (and
  // "present" for outputs): with every constant set, `int(F) == 2`, `bool(F)`, `uint(F)`, `short(H)`
  // (AIR conversion calls, which neither our evaluator nor LLVM's folded) gave ours [0, 1, 3] where
  // Metal has [] (fixtures conv_pred_allset*, regression test conv_predicates).
  bool gatedPresent(const ArgInfo &a, const char *what) {
    int p = predValue(a.predGlobal);
    if (p < 0)
      die(std::string(what) + " " + (a.argName.empty() ? a.kind : a.argName) + " is gated by " + a.predGlobal +
          ", which the static initialiser does not decide" + (spec.ctorWhy.empty() ? std::string() : " (" + spec.ctorWhy + ")") +
          ": refused rather than guessing its presence (BINDING-ABI.md 3.7)");
    return p != 0;
  }
  int locValue(const ArgInfo &a) {
    if (a.locGlobal.empty()) return a.location;
    if (spec.taintedGlobals.count(a.locGlobal)) die("binding index expression " + a.locGlobal + " depends on an unset function constant");
    auto it = spec.implicitVals.find(a.locGlobal);
    if (it == spec.implicitVals.end()) die("binding index expression " + a.locGlobal + " could not be evaluated from the function constants");
    return (int)it->second;
  }

  void setupArgs();
  // ---- texture ops ----
  int texOf(Value *V) { auto it = tex.find(V); if (it == tex.end()) die("texture operand not an argument"); return it->second; }
  int smpOf(Value *V) {
    auto it = smp.find(V); if (it != smp.end()) return it->second;
    if (auto *CI = dyn_cast<CallInst>(V)) if (CI->getCalledFunction() && CI->getCalledFunction()->getName().starts_with("air.get_read_sampler")) return -1;
    if (auto *GV = dyn_cast<GlobalVariable>(V->stripPointerCasts())) {
      auto c = constSamplers.find(GV); if (c != constSamplers.end()) return c->second;
      die("constexpr sampler " + GV->getName().str() + " was not assigned a slot");
    }
    die("sampler operand");
  }
  std::vector<nir_def *> texop(nir_texop op, glsl_sampler_dim dim, bool array, int t, int sm, nir_def *coord,
                               nir_def *lod, nir_def *bias, nir_def *offset, nir_def *minlod,
                               nir_def *ddx, nir_def *ddy, unsigned bs, nir_alu_type dt, unsigned ncomp = 4);
  bool isZeroConst(Value *v) { auto *c = dyn_cast<Constant>(v); return c && c->isNullValue(); }

  std::vector<nir_def *> callAir(CallInst *CI);

  std::vector<nir_def *> callLLVM(CallInst *CI);

  void inst(Instruction &I);

  void storePhis(BasicBlock *from, BasicBlock *to) {
    for (PHINode &PN : to->phis()) {
      if (auto pit = ptrPhis.find(&PN); pit != ptrPhis.end()) { storePtrPhi(pit->second, PN.getIncomingValueForBlock(from), &PN); continue; }
      auto v = get(PN.getIncomingValueForBlock(from));
      nir_store_var(&b, phiVars.at(&PN), vec(v), (1u << v.size()) - 1);
    }
  }

  void terminator(Instruction *T);


  nir_block *newBlock() {
    nir_block *n = nir_block_create(b.impl);
    exec_list_push_tail(&b.impl->body, &n->cf_node.node);
    n->cf_node.parent = &b.impl->cf_node;
    return n;
  }

  nir_shader *run(const nir_shader_compiler_options *opts);
};
