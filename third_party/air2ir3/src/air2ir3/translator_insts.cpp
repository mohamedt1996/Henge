// SPDX-License-Identifier: MIT
// Translator members defined out of class: instructions, terminators and the translation driver
// (inst, terminator, run).
// Part of src/air2ir3.cpp's translation unit, #included from it in order; not compiled on its own.

// ---- instructions ----
inline void Translator::inst(Instruction &I) {
  auto set = [&](std::vector<nir_def *> v) {
    std::vector<Type *> ts; flatten(I.getType(), ts);
    if (ts.size() != v.size()) die("component count mismatch (" + std::to_string(v.size()) + " vs " + std::to_string(ts.size()) + ") for " + str(&I));
    for (unsigned k = 0; k < v.size(); k++) if (!ts[k]->isPointerTy() && v[k]->bit_size != bits(ts[k])) die("bit size mismatch comp " + std::to_string(k) + " for " + str(&I));
    vals[&I] = std::move(v); };
  if (auto *BO = dyn_cast<BinaryOperator>(&I)) {
    auto x = get(BO->getOperand(0)), y = get(BO->getOperand(1)); std::vector<nir_def *> r;
    for (unsigned c = 0; c < x.size(); c++) {
      nir_def *a = x[c], *d = y[c], *o;
      switch (BO->getOpcode()) {
      case Instruction::FAdd: o = nir_fadd(&b, a, d); break; case Instruction::FSub: o = nir_fsub(&b, a, d); break;
      case Instruction::FMul: o = nir_fmul(&b, a, d); break; case Instruction::FDiv: o = nir_fdiv(&b, a, d); break;
      case Instruction::FRem: o = nir_fsub(&b, a, nir_fmul(&b, d, nir_ftrunc(&b, nir_fdiv(&b, a, d)))); break;
      case Instruction::Add: o = nir_iadd(&b, a, d); break; case Instruction::Sub: o = nir_isub(&b, a, d); break;
      case Instruction::Mul: o = nir_imul(&b, a, d); break; case Instruction::UDiv: o = nir_udiv(&b, a, d); break;
      case Instruction::SDiv: o = nir_idiv(&b, a, d); break; case Instruction::URem: o = nir_umod(&b, a, d); break;
      case Instruction::SRem: o = nir_irem(&b, a, d); break;
      case Instruction::Shl: o = nir_ishl(&b, a, nir_u2u32(&b, d)); break; case Instruction::LShr: o = nir_ushr(&b, a, nir_u2u32(&b, d)); break;
      case Instruction::AShr: o = nir_ishr(&b, a, nir_u2u32(&b, d)); break;
      case Instruction::And: o = nir_iand(&b, a, d); break; case Instruction::Or: o = nir_ior(&b, a, d); break;
      case Instruction::Xor: o = nir_ixor(&b, a, d); break;
      default: die("binop " + str(&I));
      }
      r.push_back(o);
    }
    return set(r);
  }
  if (auto *UO = dyn_cast<UnaryOperator>(&I)) { std::vector<nir_def *> r; for (nir_def *a : get(UO->getOperand(0))) r.push_back(nir_fneg(&b, a)); return set(r); }
  if (auto *C = dyn_cast<CmpInst>(&I)) {
    auto x = get(C->getOperand(0)), y = get(C->getOperand(1)); std::vector<nir_def *> r;
    for (unsigned c = 0; c < x.size(); c++) {
      nir_def *a = x[c], *d = y[c], *o;
      switch (C->getPredicate()) {
      case CmpInst::FCMP_OEQ: case CmpInst::FCMP_UEQ: o = nir_feq(&b, a, d); break;
      case CmpInst::FCMP_ONE: case CmpInst::FCMP_UNE: o = nir_fneu(&b, a, d); break;
      case CmpInst::FCMP_OLT: case CmpInst::FCMP_ULT: o = nir_flt(&b, a, d); break;
      case CmpInst::FCMP_OLE: case CmpInst::FCMP_ULE: o = nir_fge(&b, d, a); break;
      case CmpInst::FCMP_OGT: case CmpInst::FCMP_UGT: o = nir_flt(&b, d, a); break;
      case CmpInst::FCMP_OGE: case CmpInst::FCMP_UGE: o = nir_fge(&b, a, d); break;
      case CmpInst::FCMP_ORD: o = nir_iand(&b, nir_feq(&b, a, a), nir_feq(&b, d, d)); break;
      case CmpInst::FCMP_UNO: o = nir_ior(&b, nir_fneu(&b, a, a), nir_fneu(&b, d, d)); break;
      case CmpInst::FCMP_TRUE: o = nir_imm_true(&b); break; case CmpInst::FCMP_FALSE: o = nir_imm_false(&b); break;
      case CmpInst::ICMP_EQ: o = nir_ieq(&b, a, d); break; case CmpInst::ICMP_NE: o = nir_ine(&b, a, d); break;
      case CmpInst::ICMP_ULT: o = nir_ult(&b, a, d); break; case CmpInst::ICMP_ULE: o = nir_uge(&b, d, a); break;
      case CmpInst::ICMP_UGT: o = nir_ult(&b, d, a); break; case CmpInst::ICMP_UGE: o = nir_uge(&b, a, d); break;
      case CmpInst::ICMP_SLT: o = nir_ilt(&b, a, d); break; case CmpInst::ICMP_SLE: o = nir_ige(&b, d, a); break;
      case CmpInst::ICMP_SGT: o = nir_ilt(&b, d, a); break; case CmpInst::ICMP_SGE: o = nir_ige(&b, a, d); break;
      default: die("cmp " + str(&I));
      }
      r.push_back(o);
    }
    return set(r);
  }
  if (auto *S = dyn_cast<SelectInst>(&I); S && S->getType()->isPointerTy()) {
    Value *TV = S->getTrueValue(), *FV = S->getFalseValue();
    if (isa<UndefValue>(TV) || isa<UndefValue>(FV)) { ptrs[&I] = ptr(isa<UndefValue>(TV) ? FV : TV); return; }
    nir_def *c = get1(S->getCondition());
    PtrVal t = ptr(TV), f = ptr(FV);
    if (sameBase(t, f)) { PtrVal p = t; p.off = nir_bcsel(&b, c, t.off, f.off); ptrs[&I] = p; return; }
    if (t.kind == PtrVal::GLOBAL && f.kind == PtrVal::GLOBAL && t.addr64 && f.addr64) {
      PtrVal p{PtrVal::GLOBAL}; p.addr64 = nir_bcsel(&b, c, globalAddress(t), globalAddress(f)); p.off = nir_imm_int(&b, 0);
      ptrs[&I] = p; return;
    }
    die("select of pointers into different buffers or address spaces: " + str(&I));
  }
  if (auto *S = dyn_cast<SelectInst>(&I)) {
    auto c = get(S->getCondition()), t = get(S->getTrueValue()), f = get(S->getFalseValue()); std::vector<nir_def *> r;
    for (unsigned i = 0; i < t.size(); i++) r.push_back(nir_bcsel(&b, c.size() == 1 ? c[0] : c[i], t[i], f[i]));
    return set(r);
  }
  if (auto *CI = dyn_cast<CastInst>(&I)) {
    Type *DT = CI->getDestTy();
    if (DT->isPointerTy() && (CI->getOpcode() == Instruction::BitCast || CI->getOpcode() == Instruction::AddrSpaceCast)) {
      ptrs[&I] = ptr(CI->getOperand(0)); return;
    }
    auto x = get(CI->getOperand(0)); unsigned db = bits(DT->getScalarType()); std::vector<nir_def *> r;
    if (CI->getOpcode() == Instruction::BitCast || CI->getOpcode() == Instruction::AddrSpaceCast) {
      unsigned n = nflat(DT);
      if (x.size() == n && x[0]->bit_size == db) return set(x);
      nir_def *packed = nir_extract_bits(&b, x.data(), x.size(), 0, n, db);
      return set(split(packed));
    }
    for (nir_def *a : x) {
      nir_def *o;
      switch (CI->getOpcode()) {
      case Instruction::FPTrunc: case Instruction::FPExt: o = nir_f2fN(&b, a, db); break;
      case Instruction::SIToFP: o = a->bit_size == 1 ? nir_fneg(&b, nir_b2fN(&b, a, db)) : nir_i2fN(&b, a, db); break;
      case Instruction::UIToFP: o = a->bit_size == 1 ? nir_b2fN(&b, a, db) : nir_u2fN(&b, a, db); break;
      case Instruction::FPToSI: case Instruction::FPToUI:
        o = db == 1 ? nir_fneu_imm(&b, nir_ftrunc(&b, a), 0.0)
                    : CI->getOpcode() == Instruction::FPToSI ? nir_f2iN(&b, a, db) : nir_f2uN(&b, a, db); break;
      case Instruction::ZExt: o = a->bit_size == 1 ? nir_b2iN(&b, a, db) : nir_u2uN(&b, a, db); break;
      case Instruction::SExt: o = a->bit_size == 1 ? nir_ineg(&b, nir_b2iN(&b, a, db)) : nir_i2iN(&b, a, db); break;
      case Instruction::Trunc: o = db == 1 ? nir_ine_imm(&b, nir_iand_imm(&b, a, 1), 0) : nir_u2uN(&b, a, db); break;
      default: die("cast " + str(&I));
      }
      r.push_back(o);
    }
    return set(r);
  }
  if (auto *EE = dyn_cast<ExtractElementInst>(&I)) {
    auto v = get(EE->getVectorOperand());
    if (auto *ci = dyn_cast<ConstantInt>(EE->getIndexOperand())) return set({v[ci->getZExtValue()]});
    nir_def *idx = get1(EE->getIndexOperand()), *o = v[0];
    for (unsigned i = 1; i < v.size(); i++) o = nir_bcsel(&b, nir_ieq_imm(&b, idx, i), v[i], o);
    return set({o});
  }
  if (auto *IE = dyn_cast<InsertElementInst>(&I)) {
    auto v = get(IE->getOperand(0)); nir_def *e = get1(IE->getOperand(1));
    if (auto *ci = dyn_cast<ConstantInt>(IE->getOperand(2))) { v[ci->getZExtValue()] = e; return set(v); }
    nir_def *idx = get1(IE->getOperand(2));             // dynamic index: per-component select
    for (unsigned i = 0; i < v.size(); i++) v[i] = nir_bcsel(&b, nir_ieq_imm(&b, idx, i), e, v[i]);
    return set(v);
  }
  if (auto *SV = dyn_cast<ShuffleVectorInst>(&I)) {
    auto x = get(SV->getOperand(0)), y = get(SV->getOperand(1)); std::vector<nir_def *> r;
    for (int m : SV->getShuffleMask())
      r.push_back(m < 0 ? nir_undef(&b, 1, x[0]->bit_size) : (unsigned)m < x.size() ? x[m] : y[m - x.size()]);
    return set(r);
  }
  if (auto *EV = dyn_cast<ExtractValueInst>(&I)) {
    auto v = get(EV->getAggregateOperand()); Type *T = EV->getAggregateOperand()->getType(); unsigned off = 0;
    for (unsigned idx : EV->getIndices()) {
      if (auto *ST = dyn_cast<StructType>(T)) { for (unsigned i = 0; i < idx; i++) off += nflat(ST->getElementType(i)); T = ST->getElementType(idx); }
      else { T = T->getArrayElementType(); off += idx * nflat(T); }
    }
    return set(std::vector<nir_def *>(v.begin() + off, v.begin() + off + nflat(T)));
  }
  if (auto *IV = dyn_cast<InsertValueInst>(&I)) {
    auto v = get(IV->getAggregateOperand()), e = get(IV->getInsertedValueOperand()); Type *T = IV->getType(); unsigned off = 0;
    for (unsigned idx : IV->getIndices()) {
      if (auto *ST = dyn_cast<StructType>(T)) { for (unsigned i = 0; i < idx; i++) off += nflat(ST->getElementType(i)); T = ST->getElementType(idx); }
      else { T = T->getArrayElementType(); off += idx * nflat(T); }
    }
    for (unsigned i = 0; i < e.size(); i++) v[off + i] = e[i];
    return set(v);
  }
  if (isa<GetElementPtrInst>(&I)) { ptrs[&I] = ptr(&I); return; }
  if (auto *LI = dyn_cast<LoadInst>(&I)) {
    PtrVal p = ptr(LI->getPointerOperand());
    if (p.kind == PtrVal::SAMPLER_CONST) die("load from constexpr sampler");
    std::vector<nir_def *> out; loadScalars(p, LI->getType(), 0, LI->getAlign().value(), out); return set(out);
  }
  if (auto *CI = dyn_cast<CallInst>(&I)) {
    Function *Fn = CI->getCalledFunction();
    if (!Fn) die("indirect call");
    if (Fn->getName().starts_with("air.get_read_sampler")) { smp[&I] = -1; return; }
    if (Fn->getName().starts_with("air.")) { auto r = callAir(CI); if (!CI->getType()->isVoidTy()) set(r); return; }
    if (Fn->isIntrinsic()) { auto r = callLLVM(CI); if (!CI->getType()->isVoidTy()) set(r); return; }
    die("call to non-inlined function " + Fn->getName().str());
  }
  if (auto *PN = dyn_cast<PHINode>(&I)) {
    if (auto pit = ptrPhis.find(PN); pit != ptrPhis.end()) { ptrs[&I] = ptrPhiValue(pit->second); return; }
    std::vector<nir_def *> r; unsigned n = nflat(PN->getType());
    nir_variable *v = phiVars.at(PN);
    nir_def *l = nir_load_var(&b, v);
    for (unsigned i = 0; i < n; i++) r.push_back(nir_channel(&b, l, i));
    return set(r);
  }
  if (auto *SI = dyn_cast<StoreInst>(&I)) {
    PtrVal p = ptr(SI->getPointerOperand()); auto v = get(SI->getValueOperand()); unsigned idx = 0;
    storeScalars(p, SI->getValueOperand()->getType(), v, idx, 0, SI->getAlign().value()); return;
  }
  if (auto *AI = dyn_cast<AllocaInst>(&I)) {
    if (!AI->isStaticAlloca() || AI->getAddressSpace() != 0 || !AI->getAllocatedType()->isSized())
      die("unsupported dynamic, non-entry or non-private alloca: " + str(&I));
    TypeSize elem = DL.getTypeAllocSize(AI->getAllocatedType());
    constexpr uint64_t maxBytes = 65536;
    uint64_t count = cast<ConstantInt>(AI->getArraySize())->getLimitedValue(maxBytes + 1);
    if (elem.isScalable() || elem.getFixedValue() > maxBytes || count > maxBytes ||
        (count && elem.getFixedValue() > (maxBytes - allocaBytes) / count))
      die("private allocations exceed 64 KiB or use scalable types: " + str(&I));
    uint64_t size = std::max<uint64_t>(1, elem.getFixedValue() * count);
    if (size > maxBytes - allocaBytes) die("private allocations exceed 64 KiB: " + str(&I));
    allocaBytes += size;
    PtrVal p{PtrVal::PRIVATE};
    p.var = nir_local_variable_create(b.impl, glsl_array_type(glsl_uint8_t_type(), size, 1), AI->getName().str().c_str());
    p.off = nir_imm_int(&b, 0);
    ptrs[AI] = p;
    return;
  }
  die("instruction " + str(&I));
}

inline void Translator::terminator(Instruction *T) {
  if (auto *R = dyn_cast<ReturnInst>(T)) {
    if (Value *rv = R->getReturnValue()) {
      auto v = get(rv);
      std::map<nir_variable *, std::vector<nir_def *>> per;
      if (v.size() != outs.size()) die("return value does not match the output metadata");
      for (unsigned i = 0; i < outs.size(); i++) {
        if (!outs[i].first) continue;                 // function-constant-gated output switched off
        nir_def *d = v[i]; unsigned vb = glsl_get_bit_size(glsl_without_array(outs[i].first->type));
        if (d->bit_size != vb) d = glsl_base_type_is_integer(glsl_get_base_type(outs[i].first->type)) ? nir_u2uN(&b, d, vb) : nir_f2fN(&b, d, vb);
        per[outs[i].first].push_back(d);
      }
      for (auto &kv : per) {
        int ri = kv.first->data.location - FRAG_RESULT_DATA0;
        if (stage == MESA_SHADER_FRAGMENT && ri >= 0 && ri < ABI_MAX_RTS && rtLow.count(ri) && kv.first->data.index == 0) {
          nir_store_var(&b, kv.first, xrEncodeOutput(rtLow[ri], kv.second), 0xf);
          continue;
        }
        nir_store_var(&b, kv.first, vec(kv.second), (1u << kv.second.size()) - 1);
      }
    }
    nir_goto(&b, b.impl->end_block);
    return;
  }
  if (auto *Br = dyn_cast<BranchInst>(T)) {
    BasicBlock *cur = Br->getParent();
    if (Br->isUnconditional()) { storePhis(cur, Br->getSuccessor(0)); nir_goto(&b, blocks.at(Br->getSuccessor(0))); return; }
    nir_def *c = get1(Br->getCondition());
    BasicBlock *t = Br->getSuccessor(0), *f = Br->getSuccessor(1);
    bool tp = !t->phis().empty(), fp = !f->phis().empty();
    if (!tp && !fp) { nir_goto_if(&b, blocks.at(t), c, blocks.at(f)); return; }
    // Edge blocks carry the phi stores.
    nir_block *te = newBlock(), *fe = newBlock();
    nir_goto_if(&b, te, c, fe);
    b.cursor = nir_after_block(te); storePhis(cur, t); nir_goto(&b, blocks.at(t));
    b.cursor = nir_after_block(fe); storePhis(cur, f); nir_goto(&b, blocks.at(f));
    return;
  }
  if (auto *SW = dyn_cast<SwitchInst>(T)) {
    nir_def *c = get1(SW->getCondition());
    BasicBlock *cur = SW->getParent();
    for (auto &Case : SW->cases()) {
      nir_block *hit = newBlock(), *next = newBlock();
      nir_goto_if(&b, hit, nir_ieq_imm(&b, c, Case.getCaseValue()->getSExtValue()), next);
      b.cursor = nir_after_block(hit); storePhis(cur, Case.getCaseSuccessor()); nir_goto(&b, blocks.at(Case.getCaseSuccessor()));
      b.cursor = nir_after_block(next);
    }
    storePhis(cur, SW->getDefaultDest()); nir_goto(&b, blocks.at(SW->getDefaultDest()));
    return;
  }
  if (isa<UnreachableInst>(T)) { nir_goto(&b, b.impl->end_block); return; }
  die("terminator " + str(T));
}

inline nir_shader *Translator::run(const nir_shader_compiler_options *opts) {
  b = nir_builder_init_simple_shader(stage, opts, "%s", F->getName().str().c_str());
  s = b.shader;
  b.impl->structured = false;
  setupArgs();
  // phi temporaries
  for (BasicBlock &BB : *F)
    for (PHINode &PN : BB.phis()) {
      std::vector<Type *> ts; flatten(PN.getType(), ts);
      if (PN.getType()->isPointerTy()) { ptrPhis[&PN] = ptrPhiOf(&PN); continue; }
      for (Type *E : ts) if (E->isPointerTy()) die("phi of an aggregate holding pointers: " + str(&PN));
      for (Type *E : ts) if (bits(E) != bits(ts[0]) || E->isFloatingPointTy() != ts[0]->isFloatingPointTy())
        die("phi of a mixed-type aggregate: " + str(&PN));
      phiVars[&PN] = nir_local_variable_create(b.impl, glsl_vector_type(baseOf(ts[0]), ts.size()), "phi");
    }
  // blocks: the LLVM entry continues the start block; the others are new unstructured blocks
  bool first = true;
  for (BasicBlock &BB : *F) { blocks[&BB] = first ? nir_start_block(b.impl) : newBlock(); first = false; }
  ReversePostOrderTraversal<Function *> RPOT(F);        // defs before uses
  for (BasicBlock *BB : RPOT) {
    b.cursor = nir_after_block(blocks[BB]);
    for (Instruction &I : *BB) { if (I.isTerminator()) terminator(&I); else inst(I); }
  }
  // BINDING-ABI.md 3.9 (ABI v3): ir3 appends its driver UBOs at info.num_ubos.  Counting every
  // constant buffer the function DECLARES (used or not, present or not) keeps them clear of any
  // buffer(n) the app may bind: verify2's dpk.metal declares an unread buffer(1), and ABI v3 put
  // driver_params at UBO 1 on top of it.
  s->info.num_ubos = std::max(maxUbo, R.declared_ubo_max) + 1;
  s->info.shared_size = sharedSize;
  if (!constData.empty()) { s->constant_data_size = constData.size(); s->constant_data = ralloc_size(s, constData.size()); memcpy(s->constant_data, constData.data(), constData.size()); }
  if (stage == MESA_SHADER_COMPUTE) s->info.workgroup_size_variable = true;
  nir_progress(true, b.impl, nir_metadata_none);
  nir_sort_unstructured_blocks(b.impl);
  nir_index_blocks(b.impl);
  nir_validate_shader(s, "after AIR translation");
  NIR_PASS(_, s, nir_lower_goto_ifs);
  NIR_PASS(_, s, nir_lower_returns);
  NIR_PASS(_, s, nir_lower_vars_to_ssa);
  NIR_PASS(_, s, nir_opt_dce);
  nir_validate_shader(s, "after structurisation");
  R.constant_data_bytes = constData.size();
  R.shared_bytes = sharedSize;
  return s;
}
