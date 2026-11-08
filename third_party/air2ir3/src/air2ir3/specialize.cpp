// SPDX-License-Identifier: MIT
// Unset constants in the static initialiser (ABI v5), presence verdicts and specialize_and_inline.
// Part of src/air2ir3.cpp's translation unit, #included from it in order; not compiled on its own.

// ---------------------------------------- unset constants in the static initialiser (ABI v5) --
// The static initialiser (llvm.global_ctors) computes the presence predicates (__metal_implicit_fc_pred_N),
// the binding-index expressions and the constant shadows (_ZL<name>) the body reads.  ABI v5 evaluates
// it with the value-independence model of uEval (every use of an unset constant any value of its type,
// a branch on an undefined condition explores both edges): a global comes out as a constant only when
// it has that value for every value of the unset constants.  verify4 item 2: fix round 3 took the
// predicates from Metal's undef folding (abstract_eval_ctors), and fcrand seed 23 shader 38
// (work/fc/pmin/p0.metal, `Q0 = (A || (N*2)==1) ? (A != ((N&1)!=0)) : (((N>>1)>0) && B)` with only A
// unset) came out absent where Metal has buffer(10) present.
struct IV { enum K { Unknown, C, T } k = Unknown; Constant *c = nullptr; UAbs a; };
static IV ivC(Constant *c) {
  IV v;
  if (!c || isa<UndefValue>(c) || c->containsUndefOrPoisonElement()) { v.k = IV::T; v.a = c ? uFull(c->getType()) : UAbs(); v.a.poison = c && isa<PoisonValue>(c); return v; }
  v.k = IV::C; v.c = c; return v;
}
static IV ivT(const UAbs &a) { IV v; v.k = IV::T; v.a = a; return v; }
static UAbs ivAbs(const IV &v) {
  if (v.k == IV::T) return v.a;
  UAbs a; if (auto *ci = dyn_cast_or_null<ConstantInt>(v.c)) { a.hasRange = true; a.cr = ConstantRange(ci->getValue()); }
  return a;
}
static IV ivJoin(const IV &x, const IV &y) {
  if (x.k == IV::Unknown) return y;
  if (y.k == IV::Unknown) return x;
  if (x.k == IV::C && y.k == IV::C && x.c == y.c) return x;
  return ivT(uJoin(ivAbs(x), ivAbs(y)));
}
static bool ivSame(const IV &x, const IV &y) {
  if (x.k != y.k) return false;
  if (x.k == IV::C) return x.c == y.c;
  if (x.k == IV::T) return x.a.poison == y.a.poison && x.a.hasRange == y.a.hasRange && (!x.a.hasRange || x.a.cr == y.a.cr);
  return true;
}
struct CtorIndep { bool ok = true; std::string why; std::map<const GlobalVariable *, IV> mem; };
static CtorIndep indep_eval_ctors(Module &M, const std::map<int, std::vector<double>> &fc) {
  CtorIndep E;
  GlobalVariable *ctors = M.getGlobalVariable("llvm.global_ctors");
  if (!ctors || !ctors->hasInitializer()) return E;
  auto *CA = dyn_cast<ConstantArray>(ctors->getInitializer());
  if (!CA) { E.ok = false; E.why = "global_ctors not an array"; return E; }
  const DataLayout &DL = M.getDataLayout();
  TargetLibraryInfoImpl TLII(M.getTargetTriple()); TargetLibraryInfo TLI(TLII);
  std::vector<Function *> fns;
  for (Use &U : CA->operands()) {
    auto *CS = dyn_cast<ConstantStruct>(U.get()); if (!CS) continue;
    auto *Fn = dyn_cast<Function>(CS->getOperand(1)->stripPointerCasts()); if (!Fn || Fn->isDeclaration()) continue;
    fns.push_back(Fn);
  }
  // A global stored at one site that runs on every path keeps the stored value; otherwise (several
  // sites, or a conditional one) it may also keep its initialiser or an earlier store.
  std::map<const GlobalVariable *, int> sites; std::set<const StoreInst *> always;
  for (Function *Fn : fns) {
    PostDominatorTree PDT(*Fn);
    for (BasicBlock &BB : *Fn) for (Instruction &I : BB) if (auto *SI = dyn_cast<StoreInst>(&I))
      if (auto *G = dyn_cast<GlobalVariable>(SI->getPointerOperand()->stripPointerCasts())) {
        sites[G]++;
        if (PDT.dominates(&BB, &Fn->getEntryBlock())) always.insert(SI);
      }
  }
  auto initOf = [&](const GlobalVariable *G, Type *T) -> IV {
    if (G->hasInitializer() && G->getInitializer()->getType() == T) return ivC(const_cast<Constant *>(G->getInitializer()));
    return ivT(uFull(T));
  };
  for (Function *Fn : fns) {
    std::map<const Value *, IV> val;
    std::set<std::pair<const BasicBlock *, const BasicBlock *>> edges;
    std::set<const BasicBlock *> live{&Fn->getEntryBlock()};
    auto get = [&](Value *v) -> IV {
      if (auto *C = dyn_cast<Constant>(v)) { if (isa<GlobalValue>(C)) return ivT(uFull(v->getType())); return ivC(C); }
      auto it = val.find(v); return it == val.end() ? IV() : it->second;
    };
    SimplifyQuery SQ(DL, &TLI);
    ReversePostOrderTraversal<Function *> RPOT(Fn);
    for (int iter = 0; iter < 64; iter++) {
      bool changed = false;
      auto setv = [&](Instruction *I, const IV &v) {
        if (v.k == IV::Unknown) return;
        IV o = val.count(I) ? val[I] : IV(); IV n = ivJoin(o, v);
        if (!ivSame(o, n)) { val[I] = n; changed = true; }
      };
      auto edge = [&](BasicBlock *f, BasicBlock *t) { if (edges.insert({f, t}).second) changed = true; if (live.insert(t).second) changed = true; };
      for (BasicBlock *BB : RPOT) {
        if (!live.count(BB)) continue;
        for (Instruction &I : *BB) {
          if (auto *PN = dyn_cast<PHINode>(&I)) {
            IV r;
            for (unsigned i = 0; i < PN->getNumIncomingValues(); i++)
              if (edges.count({PN->getIncomingBlock(i), BB})) r = ivJoin(r, get(PN->getIncomingValue(i)));
            setv(&I, r); continue;
          }
          if (auto *LI = dyn_cast<LoadInst>(&I)) {
            auto *G = dyn_cast<GlobalVariable>(LI->getPointerOperand()->stripPointerCasts());
            if (!G) { setv(&I, ivT(uFull(LI->getType()))); continue; }
            if (G->getSection() == "air.fc_initializer") {
              int idx = fcIndexOfGlobal(G->getName());
              setv(&I, idx >= 0 && fc.count(idx) ? ivC(fcLoadConstant(G, LI->getType(), fc.at(idx), DL)) : ivT(uFull(LI->getType())));
              continue;
            }
            auto m = E.mem.find(G);
            if (m != E.mem.end()) {
              if (m->second.k == IV::C) { Constant *pc = punStored(m->second.c, LI->getType(), DL); setv(&I, pc ? ivC(pc) : ivT(uFull(LI->getType()))); }
              else setv(&I, m->second);
              continue;
            }
            setv(&I, initOf(G, LI->getType()));
            continue;
          }
          if (auto *SI = dyn_cast<StoreInst>(&I)) {
            auto *G = dyn_cast<GlobalVariable>(SI->getPointerOperand()->stripPointerCasts());
            if (!G) continue;
            IV v = get(SI->getValueOperand());
            if (v.k == IV::Unknown) continue;
            auto it = E.mem.find(G);
            IV slot = it != E.mem.end() ? it->second : initOf(G, SI->getValueOperand()->getType());
            IV n = sites[G] == 1 && always.count(SI) ? v : ivJoin(slot, v);
            if (it == E.mem.end() || !ivSame(it->second, n)) { E.mem[G] = n; changed = true; }
            continue;
          }
          if (auto *CI = dyn_cast<CallInst>(&I)) {
            Function *cf = CI->getCalledFunction();
            StringRef nm = cf ? cf->getName() : "";
            if (nm.starts_with("air.normalize_function_constant_predicate")) {
              IV x = get(argOperand(CI, 0));
              if (x.k == IV::Unknown) continue;
              if (x.k == IV::C) { auto *ci = dyn_cast<ConstantInt>(x.c); setv(&I, ivC(ConstantInt::get(CI->getType(), ci ? !ci->isZero() : 0))); continue; }
              unsigned w = CI->getType()->getIntegerBitWidth();
              if (x.a.hasRange && !x.a.poison && !x.a.cr.contains(APInt::getZero(x.a.cr.getBitWidth()))) { setv(&I, ivC(ConstantInt::get(CI->getType(), 1))); continue; }
              UAbs a; a.hasRange = true; a.cr = ConstantRange(APInt(w, 0), APInt(w, 2)); a.poison = x.a.poison;
              setv(&I, ivT(a)); continue;
            }
            if (nm == "air.is_function_constant_defined") {
              auto *G = dyn_cast<GlobalVariable>(argOperand(CI, 0)->stripPointerCasts());
              int idx = G ? fcIndexOfGlobal(G->getName()) : -1;
              setv(&I, ivC(ConstantInt::getBool(CI->getContext(), idx >= 0 && fc.count(idx))));
              continue;
            }
            if (cf && cf->isIntrinsic() && (cf->getIntrinsicID() == Intrinsic::lifetime_start || cf->getIntrinsicID() == Intrinsic::lifetime_end)) continue;
            if (cf && cf->isDeclaration() && nm.starts_with("air.")) {
              // fix round 5 (verify5 minor): an AIR conversion of set constants folds as in
              // abstract_eval_ctors; on a value that depends on an unset constant (or when it does not
              // fold) its result may be any value of its type, and poison for a float -> integer
              // conversion (out of range / NaN).  v4 refused the variant for any call.  (Any other AIR
              // call leaves abstract_eval_ctors unevaluated, and the variant is refused there.)
              SmallVector<Constant *, 3> args; bool unk = false, dep = false;
              for (Value *op : CI->args()) { IV x = get(op); if (x.k == IV::Unknown) unk = true; else if (x.k == IV::T) dep = true; else args.push_back(x.c); }
              if (unk) continue;
              if (!dep) if (Constant *r = foldCtorAirCall(CI, args)) { setv(&I, ivC(r)); continue; }
              UAbs a = uFull(CI->getType());
              Type *RT = CI->getType();
              if (nm.starts_with("air.convert.") && RT->isIntegerTy() && !RT->isIntegerTy(1) && CI->arg_size() == 1 && argOperand(CI, 0)->getType()->isFPOrFPVectorTy())
                a.poison = true;
              setv(&I, ivT(a));
              continue;
            }
            E.ok = false; E.why = "call to " + nm.str() + " in the static initialiser"; return E;
          }
          if (auto *Br = dyn_cast<BranchInst>(&I)) {
            if (Br->isUnconditional()) { edge(BB, Br->getSuccessor(0)); continue; }
            IV c = get(Br->getCondition());
            if (c.k == IV::Unknown) continue;
            if (c.k == IV::T) { edge(BB, Br->getSuccessor(0)); edge(BB, Br->getSuccessor(1)); continue; }   // either way
            edge(BB, cast<ConstantInt>(c.c)->isOne() ? Br->getSuccessor(0) : Br->getSuccessor(1));
            continue;
          }
          if (auto *SW = dyn_cast<SwitchInst>(&I)) {
            IV c = get(SW->getCondition());
            if (c.k == IV::Unknown) continue;
            if (c.k == IV::T) { for (unsigned i = 0; i < SW->getNumSuccessors(); i++) edge(BB, SW->getSuccessor(i)); continue; }
            edge(BB, SW->findCaseValue(cast<ConstantInt>(c.c))->getCaseSuccessor());
            continue;
          }
          if (isa<ReturnInst>(&I) || isa<UnreachableInst>(&I)) continue;
          SmallVector<UOpnd, 4> ops; std::vector<IV> ivs; bool anyUnknown = false, anyT = false;
          ivs.reserve(I.getNumOperands());
          for (Value *op : I.operands()) {
            IV v = get(op);
            if (v.k == IV::Unknown) { anyUnknown = true; break; }
            ivs.push_back(v);
          }
          if (anyUnknown) continue;
          for (unsigned i = 0; i < I.getNumOperands(); i++) {
            UOpnd o;
            if (ivs[i].k == IV::T) { o.v = I.getOperand(i); o.t = &ivs[i].a; anyT = true; }
            else o.v = ivs[i].c;
            ops.push_back(o);
          }
          if (!anyT) {
            SmallVector<Constant *, 4> cs; for (auto &o : ops) cs.push_back(cast<Constant>(o.v));
            Constant *res = ConstantFoldInstOperands(&I, cs, DL, &TLI);
            if (!res) { E.ok = false; E.why = "cannot fold " + str(&I); return E; }
            setv(&I, ivC(res));
            continue;
          }
          UResult ur = uEval(I, ops, SQ, &TLI, [&](Value *v) { IV x = get(v); return x.k == IV::T; });
          if (ur.v && ur.v != &I) { IV r = get(ur.v); if (r.k != IV::Unknown) setv(&I, r); }
          else setv(&I, ivT(ur.a));
        }
      }
      if (!changed) break;
      if (iter == 63) { E.ok = false; E.why = "static initialiser did not converge"; return E; }
    }
  }
  return E;
}
// A presence predicate that is a direct copy of an unset constant: `[[function_constant(A)]]` with
// `constant bool Q = A;` style initialisers (normalize(A), possibly through a shadow and casts / != 0).
// Metal marks such arguments absent (measured, fixtures fcsem/fc2/fc3), and that is kept.
static bool directUnsetPredicate(const GlobalVariable *P, const std::map<int, std::vector<double>> &fc, int depth = 0) {
  if (depth > 4) return false;
  const StoreInst *only = nullptr; int n = 0;
  for (const User *U : P->users()) {
    if (auto *CE = dyn_cast<ConstantExpr>(U)) { for (const User *u2 : CE->users()) if (auto *S = dyn_cast<StoreInst>(u2)) { only = S; n++; } continue; }
    if (auto *S = dyn_cast<StoreInst>(U)) { only = S; n++; }
  }
  if (n != 1 || !only) return false;
  const Value *v = only->getValueOperand();
  for (int k = 0; k < 8; k++) {
    if (auto *CI = dyn_cast<CallInst>(v)) {
      if (CI->getCalledFunction() && CI->getCalledFunction()->getName().starts_with("air.normalize_function_constant_predicate")) { v = argOperand(CI, 0); continue; }
      return false;
    }
    if (isa<ZExtInst>(v) || isa<SExtInst>(v) || isa<TruncInst>(v)) { v = cast<Instruction>(v)->getOperand(0); continue; }
    if (auto *IC = dyn_cast<ICmpInst>(v)) {
      if (IC->getPredicate() != ICmpInst::ICMP_NE) return false;
      if (auto *z = dyn_cast<ConstantInt>(IC->getOperand(1)); z && z->isZero()) { v = IC->getOperand(0); continue; }
      if (auto *z = dyn_cast<ConstantInt>(IC->getOperand(0)); z && z->isZero()) { v = IC->getOperand(1); continue; }
      return false;
    }
    break;
  }
  auto *LI = dyn_cast<LoadInst>(v); if (!LI) return false;
  auto *G = dyn_cast<GlobalVariable>(LI->getPointerOperand()->stripPointerCasts()); if (!G) return false;
  if (G->getSection() == "air.fc_initializer") { int idx = fcIndexOfGlobal(G->getName()); return idx >= 0 && !fc.count(idx); }
  return directUnsetPredicate(G, fc, depth + 1);                    // a shadow copy of the constant
}

// verify4 item 2 (ABI v5): a presence predicate that depends on an unset constant is accepted only
// when it is a direct copy of an unset constant (absent, as in Metal) or has the same value for every
// value of the unset constants (indep_eval_ctors); every other one is refused where it gates an
// argument or output of the entry point (Translator::predValue).  ABI v4 took Metal's undef folding
// for these, and Metal's choice is not reproducible (fcrand seed 23 shader 38, work/fc/pmin/p0).
static std::map<std::string, std::string> presenceVerdicts(Module &M, const CtorEval &AE, const CtorIndep &IE, const std::vector<FcDecl> &decls,
                                                          const std::map<int, std::vector<double>> &fc, std::set<std::string> &direct) {
  std::map<std::string, std::string> amb;
  auto dep = ctorDeps(M, fc);
  for (auto &kv : IE.mem) {
    const GlobalVariable *G = kv.first;
    if (!G->getName().contains("__metal_implicit_fc_pred")) continue;
    std::string g = G->getName().str(), names;
    auto it = dep.find(G);
    if (it != dep.end())
      for (int i : it->second) for (auto &d : decls) if (d.index == i) names += (names.empty() ? "" : ", ") + d.name + " (index " + std::to_string(i) + ")";
    if (kv.second.k == IV::C) {
      // guard: the value-independent result must also be what LLVM's undef folding (Metal's) gives
      auto a = AE.mem.find(G);
      bool agree = a != AE.mem.end() && a->second.k == AV::C && a->second.c == kv.second.c;
      if (!agree)
        amb[g] = "the presence predicate " + g + " is the same for every value of the unset function constant(s) " + (names.empty() ? "?" : names) +
                 " but LLVM's undef folding gives another value; set the constant(s) (BINDING-ABI.md 3.7)";
      continue;
    }
    if (kv.second.k != IV::T) continue;
    if (directUnsetPredicate(G, fc)) { direct.insert(g); continue; }
    amb[g] = "the presence predicate " + g + " depends on the value of the unset function constant(s) " + (names.empty() ? "?" : names) +
             " (it is neither a direct copy of an unset constant nor the same for every value of it); Metal resolves such a predicate "
             "either way (verify4: fcrand seed 23 shader 38): set the constant(s) (BINDING-ABI.md 3.7)";
  }
  return amb;
}

static SpecResult specialize_and_inline(Module &M, Function *entry, const std::vector<FcDecl> &decls,
                                        const std::map<int, std::vector<double>> &fc) {
  SpecResult R;
  // Metal's view of the predicates (before anything is rewritten), see abstract_eval_ctors.
  CtorEval AE = abstract_eval_ctors(M, fc);
  if (!AE.ok) R.ctorWhy = AE.why;
  bool allDefined = true;
  for (auto &d : decls) if (!fc.count(d.index)) allDefined = false;
  // ABI v5: which static-initialiser globals are the same for every value of the unset constants
  // (indep_eval_ctors), and the presence predicates that are not (presenceVerdicts).
  CtorIndep IE;
  if (!allDefined) {
    IE = indep_eval_ctors(M, fc);
    if (!IE.ok) die("cannot evaluate the static initialiser with unset constants (" + IE.why + "): set the constants");
    if (AE.ok) R.ambiguousPredicates = presenceVerdicts(M, AE, IE, decls, fc, R.directPredicates);
  }
  // The body sees unset constants as undefined values; a use folds only when its result is the
  // same for every value (resolve_unset_in_body).  Run on the AIR as loaded, before any of our passes.
  if (AE.ok && !allDefined) {
    std::set<Function *> ctorFns;
    if (GlobalVariable *ctors = M.getGlobalVariable("llvm.global_ctors"))
      if (auto *CA = dyn_cast<ConstantArray>(ctors->getInitializer()))
        for (Use &U : CA->operands())
          if (auto *CS = dyn_cast<ConstantStruct>(U.get()))
            if (auto *Fn = dyn_cast<Function>(CS->getOperand(1)->stripPointerCasts())) ctorFns.insert(Fn);
    std::map<const GlobalVariable *, KnownGlobal> known;
    for (GlobalVariable &G : M.globals()) {
      if (G.getSection() == "air.fc_initializer") {
        int idx = fcIndexOfGlobal(G.getName());
        if (idx < 0) continue;
        KnownGlobal k; k.name = fcNameOfGlobal(&G, decls);
        if (fc.count(idx)) k.value = fcConstant(G.getValueType(), fc.at(idx));
        known[&G] = k;
        continue;
      }
      // a shadow the body may load: its value when it is the same for every value of the unset
      // constants, else an undefined marker (ABI v5; v4 used Metal's undef folding here, verify4 item 1)
      auto m = IE.mem.find(&G);
      if (m == IE.mem.end() || m->second.k == IV::Unknown || G.getName().contains("__metal_implicit")) continue;
      KnownGlobal k; k.name = "shadow " + G.getName().str();
      if (m->second.k == IV::C) k.value = m->second.c;
      known[&G] = k;
    }
    // name the undefined shadows after the constant whose initialiser they copy
    for (Function *Fn : ctorFns) for (BasicBlock &BB : *Fn) for (Instruction &I : BB)
      if (auto *SI = dyn_cast<StoreInst>(&I))
        if (auto *G = dyn_cast<GlobalVariable>(SI->getPointerOperand()->stripPointerCasts()))
          if (known.count(G) && !known[G].value)
            if (auto *LI = dyn_cast<LoadInst>(SI->getValueOperand()))
              if (auto *FG = dyn_cast<GlobalVariable>(LI->getPointerOperand()->stripPointerCasts()))
                if (FG->getSection() == "air.fc_initializer") known[G].name = fcNameOfGlobal(FG, decls);
    // a shadow computed from several constants: name the unset ones it depends on
    { auto dep = ctorDeps(M, fc);
      for (auto &kv : known) {
        if (kv.second.value || kv.second.name.rfind("shadow ", 0) != 0) continue;
        auto it = dep.find(kv.first); if (it == dep.end() || it->second.empty()) continue;
        std::string n;
        for (int i : it->second) for (auto &d : decls) if (d.index == i) n += (n.empty() ? "" : " or ") + d.name + " (index " + std::to_string(i) + ")";
        if (!n.empty()) kv.second.name = n + " [through " + kv.first->getName().str() + "]";
      } }
    UnsetResolveStats us = resolve_unset_in_body(M, entry, ctorFns, known, fc);
    R.unsetMarkers = us.markers; R.unsetIndependent = us.independent;
  }
  // The static initialiser gets the predicate values the gates get (a normalize the evaluation
  // reached folds to its abstract value; undefined -> 0).  Since ABI v4 the body no longer loads an
  // undefined shadow (resolve_unset_in_body replaced or refused every such load), so the stores
  // below only keep the initialiser consistent for GlobalOpt and the Evaluator self-check.
  if (AE.ok && !allDefined) {
    for (auto &kv : AE.normalize) {
      CallInst *CI = kv.first;
      Constant *c = kv.second.k == AV::C ? kv.second.c : Constant::getNullValue(CI->getType());
      CI->replaceAllUsesWith(c); CI->eraseFromParent();
    }
    // The initialiser stores the value-independent result wherever there is one (ABI v5), and 0
    // where the value depends on an unset constant (the body never reads those: resolve_unset_in_body
    // replaced every such load by an undefined marker and refused the variant where it mattered).
    if (GlobalVariable *ctors = M.getGlobalVariable("llvm.global_ctors"))
      if (auto *CA = dyn_cast<ConstantArray>(ctors->getInitializer()))
        for (Use &U : CA->operands())
          if (auto *CS = dyn_cast<ConstantStruct>(U.get()))
            if (auto *Fn = dyn_cast<Function>(CS->getOperand(1)->stripPointerCasts()))
              for (BasicBlock &BB : *Fn) for (Instruction &I : BB)
                if (auto *SI = dyn_cast<StoreInst>(&I))
                  if (auto *G = dyn_cast<GlobalVariable>(SI->getPointerOperand()->stripPointerCasts())) {
                    auto m = IE.mem.find(G);
                    if (m == IE.mem.end() || G->getSection() == "air.fc_initializer") continue;
                    Type *T = SI->getValueOperand()->getType();
                    if (m->second.k == IV::C && m->second.c->getType() == T) SI->setOperand(0, m->second.c);
                    else if (m->second.k == IV::T && (T->isIntegerTy() || T->isFloatingPointTy()))
                      SI->setOperand(0, Constant::getNullValue(T));   // undefined -> 0 (false): absent AND not read
                  }
  }
  // Replace loads of "*.MTL_FC_INIT_<idx>_<ty>" constants with the chosen values.
  for (GlobalVariable &G : M.globals()) {
    if (G.getSection() != "air.fc_initializer") continue;
    int idx = fcIndexOfGlobal(G.getName());
    if (idx < 0) continue;
    std::vector<double> v = fc.count(idx) ? fc.at(idx) : std::vector<double>{0};
    for (User *U : make_early_inc_range(G.users()))
      if (auto *LI = dyn_cast<LoadInst>(U)) {
        LI->replaceAllUsesWith(fcLoadConstant(&G, LI->getType(), v, M.getDataLayout()));
        LI->eraseFromParent();
      }
  }
  for (auto &d : decls) R.fcDefined[d.index] = fc.count(d.index) != 0;
  // is_function_constant_defined(@fc) -> whether the caller supplied a value; fold predicates.
  for (Function &Fn : M) {
    if (!Fn.isDeclaration()) continue;
    StringRef fn = Fn.getName();
    for (User *U : make_early_inc_range(Fn.users())) {
      auto *CI = dyn_cast<CallInst>(U); if (!CI) continue;
      if (fn == "air.is_function_constant_defined") {
        auto *G = dyn_cast<GlobalVariable>(argOperand(CI, 0)->stripPointerCasts());
        int idx = G ? fcIndexOfGlobal(G->getName()) : -1;
        CI->replaceAllUsesWith(ConstantInt::getBool(CI->getContext(), fc.count(idx) != 0)); CI->eraseFromParent();
      } else if (fn.starts_with("air.normalize_function_constant_predicate")) {
        // normalize(x) == (x != 0) ? 1 : 0; expand it so instcombine / the evaluator can fold it.
        IRBuilder<> B(CI);
        Value *x = argOperand(CI, 0);
        Value *r = B.CreateZExt(B.CreateICmpNE(x, Constant::getNullValue(x->getType())), CI->getType());
        CI->replaceAllUsesWith(r); CI->eraseFromParent();
      }
    }
  }
  // verify6 must-fix 3: the static initialiser is IEEE arithmetic.  Metal decides presence with IEEE
  // values (half(70000) is inf, isfinite(inf) false), and so do abstract_eval_ctors and
  // indep_eval_ctors; with Metal's fast-math flags left on, the initialiser inlined into the body
  // folded an overflow to poison (overflow_body: output 0 where Metal has 3; absent_read: the body
  // read UBO 0 and 1, which the sidecar marks absent), and LLVM's Evaluator disagreed with ours
  // (the spurious "static-initialiser evaluation disagrees" refusal).  No WindowServer initialiser has
  // a float operation (verify6: 455 of 455), so this changes none of them.
  std::set<Function *> ctorFnSet;
  if (GlobalVariable *ctors = M.getGlobalVariable("llvm.global_ctors"))
    if (auto *CA = dyn_cast<ConstantArray>(ctors->getInitializer()))
      for (Use &U : CA->operands())
        if (auto *CS = dyn_cast<ConstantStruct>(U.get()))
          if (auto *Fn = dyn_cast<Function>(CS->getOperand(1)->stripPointerCasts()); Fn && !Fn->isDeclaration()) ctorFnSet.insert(Fn);
  for (Function *Fn : ctorFnSet)
    for (BasicBlock &BB : *Fn) for (Instruction &I : BB)
      if (isa<FPMathOperator>(&I)) I.copyFastMathFlags(FastMathFlags());   // (setFastMathFlags only ORs flags in)
  // Evaluate the static initialiser(s) on a clone: this yields the function-constant predicates
  // (__metal_implicit_fc_pred_N) and attribute index expressions (__metal_implicit_attr_int_expr_N)
  // that gate arguments/outputs and choose binding indices.
  if (GlobalVariable *ctors = M.getGlobalVariable("llvm.global_ctors")) {
    if (auto *CA = dyn_cast<ConstantArray>(ctors->getInitializer())) {
      std::unique_ptr<Module> C = CloneModule(M);
      runPipeline(*C, "function(instcombine<no-verify-fixpoint>)");
      TargetLibraryInfoImpl TLII(C->getTargetTriple());
      TargetLibraryInfo TLI(TLII);
      Evaluator Eval(C->getDataLayout(), &TLI);
      bool ok = true;
      GlobalVariable *cctors = C->getGlobalVariable("llvm.global_ctors");
      if (auto *CCA = dyn_cast<ConstantArray>(cctors->getInitializer()))
        for (Use &U : CCA->operands())
          if (auto *CS = dyn_cast<ConstantStruct>(U.get()))
            if (auto *Fn = dyn_cast<Function>(CS->getOperand(1)->stripPointerCasts())) {
              Constant *rv = nullptr; SmallVector<Constant *, 0> args;
              ok = ok && Eval.EvaluateFunction(Fn, rv, args);
            }
      R.ctorEvaluated = ok;
      // verify6 must-fix 1: when our evaluator could not decide the initialiser (a poison conversion,
      // a call Metal rejects), LLVM's Evaluator may still succeed on the instcombined clone, because
      // instcombine drops a dead conversion whose result another set constant absorbs
      // (`(uint(F) == 7u) || A` with A = true).  Metal evaluates such a predicate as poison (false), so
      // no predicate or binding index is taken from it: gatedPresent / locValue refuse instead.
      if (ok && AE.ok)
        for (auto &kv : Eval.getMutatedInitializers()) {
          if (kv.first->getName().contains("__metal_implicit"))
            if (auto *CI = dyn_cast<ConstantInt>(kv.second)) R.implicitVals[kv.first->getName().str()] = CI->getSExtValue();
        }
      (void)CA;
    }
  }
  // Implicit globals that the initialiser never writes keep their (constant) initialiser.
  bool hasCtor = M.getGlobalVariable("llvm.global_ctors") != nullptr;
  auto storedTo = [](const GlobalVariable &G) {
    for (const User *U : G.users()) {
      if (isa<StoreInst>(U)) return true;
      if (isa<ConstantExpr>(U)) for (const User *u2 : U->users()) if (isa<StoreInst>(u2)) return true;
    }
    return false;
  };
  if (!hasCtor || (R.ctorEvaluated && AE.ok))
  for (GlobalVariable &G : M.globals())
    if (G.getName().contains("__metal_implicit") && !R.implicitVals.count(G.getName().str()) && G.hasInitializer())
      if (auto *CI = dyn_cast<ConstantInt>(G.getInitializer())) R.implicitVals[G.getName().str()] = CI->getSExtValue();
  // an implicit global no store writes keeps its initialiser whether or not the initialiser evaluated
  if (hasCtor && !AE.ok)
    for (GlobalVariable &G : M.globals())
      if (G.getName().contains("__metal_implicit") && !R.implicitVals.count(G.getName().str()) && G.hasInitializer() && !storedTo(G))
        if (auto *CI = dyn_cast<ConstantInt>(G.getInitializer())) R.implicitVals[G.getName().str()] = CI->getSExtValue();

  // The predicates and binding-index expressions: with unset constants, the value-independent
  // result (ABI v5, indep_eval_ctors; a direct copy of an unset constant and everything that depends
  // on one are "undefined" -> 0, i.e. absent, and presenceVerdicts refuses the latter where it gates
  // something).  With every constant defined the abstract evaluation must agree with LLVM's
  // Evaluator: a self-check run on every compile.
  if (AE.ok) {
    for (auto &kv : AE.mem) {
      std::string g = kv.first->getName().str();
      if (g.find("__metal_implicit") == std::string::npos) continue;
      int64_t v = 0; bool undef = kv.second.k != AV::C;
      Constant *cv = undef ? nullptr : kv.second.c;
      if (!allDefined) {
        auto ie = IE.mem.find(kv.first);
        undef = ie == IE.mem.end() || ie->second.k != IV::C;
        cv = undef ? nullptr : ie->second.c;
      }
      if (!undef) { auto *ci = dyn_cast<ConstantInt>(cv); if (!ci) continue; v = ci->getSExtValue(); }
      if (undef) R.taintedGlobals.insert(g);
      if (allDefined && R.ctorEvaluated && R.implicitVals.count(g) && (undef || R.implicitVals[g] != v))
        die("internal: static-initialiser evaluation disagrees for " + g);
      if (undef && g.find("__metal_implicit_fc_pred") != std::string::npos) R.taintedPredicates++;
      R.implicitVals[g] = undef ? 0 : v;
    }
  } else if (!allDefined) {
    die("cannot evaluate function-constant predicates with unset constants (" + AE.why + ")");
  }
  // verify6 must-fix 3: with every constant set, the initialiser's stores get the evaluator's values
  // (the unset path above stores the value-independent ones), so the body reads exactly what the
  // presence decision used.  Only a global the initialisers store once (an implicit predicate, a
  // shadow): a global stored at several sites keeps its code, which is IEEE now (flags cleared above).
  if (AE.ok && allDefined) {
    std::map<const GlobalVariable *, std::vector<StoreInst *>> st;
    for (Function *Fn : ctorFnSet) for (BasicBlock &BB : *Fn) for (Instruction &I : BB)
      if (auto *SI = dyn_cast<StoreInst>(&I))
        if (auto *G = dyn_cast<GlobalVariable>(SI->getPointerOperand()->stripPointerCasts())) st[G].push_back(SI);
    for (auto &kv : st) {
      const GlobalVariable *G = kv.first;
      if (kv.second.size() != 1 || G->getSection() == "air.fc_initializer") continue;
      auto m = AE.mem.find(G);
      if (m == AE.mem.end() || m->second.k != AV::C) continue;
      StoreInst *SI = kv.second[0];
      if (m->second.c->getType() == SI->getValueOperand()->getType()) SI->setOperand(0, m->second.c);
    }
  }
  // Let GlobalOpt evaluate the (now constant) static initialisers into global initialisers.
  runPipeline(M, PIPE_PRE);
  // Run any remaining static initialisers (air.static_init) at the top of the entry point.
  if (GlobalVariable *ctors = M.getGlobalVariable("llvm.global_ctors")) {
    if (auto *CA = dyn_cast<ConstantArray>(ctors->getInitializer())) {
      IRBuilder<> B(&*entry->getEntryBlock().getFirstInsertionPt());
      for (Use &U : CA->operands())
        if (auto *CS = dyn_cast<ConstantStruct>(U.get()))
          if (auto *Fn = dyn_cast<Function>(CS->getOperand(1)->stripPointerCasts()))
            B.CreateCall(Fn);
    }
  }
  for (Function &F : M)
    if (!F.isDeclaration() && &F != entry) {
      F.removeFnAttr(Attribute::NoInline);
      F.removeFnAttr(Attribute::OptimizeNone);
      F.addFnAttr(Attribute::AlwaysInline);
    }
  runPipeline(M, PIPE_MAIN);
  // Resource handles stored into private memory that is never read back as a pointer (docs/USERSPACE-GRAPHICS-20261019.md):
  // RenderBox's custom effects copy a texture into an RB::Layer that a loop keeps in memory, while every sample uses
  // the argument itself. The translator has no private form for a handle, so drop those dead stores. Memory that is
  // read back as a pointer, copied, passed to a call or escapes keeps them (and still refuses in the translator).
  for (Function &F : M) {
    std::map<const AllocaInst *, bool> readBack;
    auto pointerReadBack = [&](const AllocaInst *A) {
      auto it = readBack.find(A);
      if (it != readBack.end()) return it->second;
      bool read = false;
      SmallVector<const Value *, 16> work{A}; std::set<const Value *> seen;
      while (!work.empty() && !read) {
        const Value *V = work.pop_back_val();
        if (!seen.insert(V).second) continue;
        for (const User *U : V->users()) {
          if (auto *L = dyn_cast<LoadInst>(U)) {
            read |= containsPointer(L->getType());
          } else if (auto *S = dyn_cast<StoreInst>(U)) {
            read |= S->getValueOperand() == V;                        // the address itself escapes
          } else if (isa<GetElementPtrInst>(U) || isa<BitCastInst>(U) || isa<AddrSpaceCastInst>(U)) {
            work.push_back(U);
          } else if (auto *II = dyn_cast<IntrinsicInst>(U)) {
            read |= !II->isLifetimeStartOrEnd();
          } else read = true;                                           // calls, phis, selects, ptrtoint, ...
        }
      }
      return readBack[A] = read;
    };
    for (BasicBlock &BB : F)
      for (Instruction &I : make_early_inc_range(BB)) {
        auto *S = dyn_cast<StoreInst>(&I);
        if (!S || !S->getValueOperand()->getType()->isPointerTy()) continue;
        const Value *base = S->getPointerOperand();
        while (true) {
          if (auto *G = dyn_cast<GEPOperator>(base)) base = G->getPointerOperand();
          else if (auto *O = dyn_cast<Operator>(base); O && (O->getOpcode() == Instruction::BitCast ||
                                                            O->getOpcode() == Instruction::AddrSpaceCast)) base = O->getOperand(0);
          else break;
        }
        if (auto *A = dyn_cast<AllocaInst>(base); A && !pointerReadBack(A)) S->eraseFromParent();
      }
  }
  // AIR intrinsics with constant operands (Metal folds them; LLVM cannot): fold and clean up again
  for (int i = 0; i < 4 && foldAirCalls(M); i++) runPipeline(M, PIPE_POST);
  // Function-constant shadows (_ZL<name>) are internal addrspace(2) globals written once by the
  // static initialiser. After inlining + GVN the loads are forwarded; forward any remaining
  // loads from the single constant store and drop the now-dead stores.
  bool changed = true;
  while (changed) {
    changed = false;
    for (GlobalVariable &G : M.globals()) {
      if (!G.hasLocalLinkage() || G.isConstant()) continue;
      SmallVector<StoreInst *, 4> stores; SmallVector<LoadInst *, 4> loads; bool other = false;
      for (User *U : G.users()) {
        if (auto *S = dyn_cast<StoreInst>(U)) stores.push_back(S);
        else if (auto *L = dyn_cast<LoadInst>(U)) loads.push_back(L);
        else if (!isa<Constant>(U)) other = true;
      }
      if (other || stores.empty()) continue;
      if (!loads.empty()) {
        bool same = isa<Constant>(stores[0]->getValueOperand());
        for (StoreInst *S : stores) same = same && S->getValueOperand() == stores[0]->getValueOperand();
        if (!same) continue;   // all stores write the same constant (the ctor can be duplicated by simplifycfg)
        for (LoadInst *L : loads)
          if (L->getType() == stores[0]->getValueOperand()->getType()) { L->replaceAllUsesWith(stores[0]->getValueOperand()); L->eraseFromParent(); }
      }
      bool anyLoad = false; for (User *U : G.users()) if (isa<LoadInst>(U)) anyLoad = true;
      if (!anyLoad) {
        // keep the value of implicit globals we could not evaluate from the initialiser -- only when
        // our evaluator succeeded (verify6 must-fix 1: after it gave up on a poison conversion, the
        // optimiser drops the dead conversion and the store here holds a value Metal does not have)
        if (AE.ok && G.getName().contains("__metal_implicit") && !R.implicitVals.count(G.getName().str()))
          if (auto *CI = dyn_cast<ConstantInt>(stores[0]->getValueOperand())) R.implicitVals[G.getName().str()] = CI->getSExtValue();
        for (StoreInst *S : stores) S->eraseFromParent();
        changed = true;
      }
    }
  }
  runPipeline(M, PIPE_POST);
  if (verifyModule(M, &errs())) die("module failed verification after specialisation");
  return R;
}
