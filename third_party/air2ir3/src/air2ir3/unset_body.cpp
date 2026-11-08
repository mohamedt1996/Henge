// SPDX-License-Identifier: MIT
// Unset function constants in the function body: AIR call folding (ABI v4) and proved value independence (ABI v5).
// Part of src/air2ir3.cpp's translation unit, #included from it in order; not compiled on its own.

// ---------------------------------------------------------- unset constants in the body (ABI v4) --
// See resolve_unset_in_body below: an unset constant may decide the static-initialiser predicates
// (presence, reproduced exactly as Metal does, abstract_eval_ctors), but not anything in the body.
// Constant folding of the AIR integer intrinsics (min/max/abs/clamp) and of int/float conversions
// with constant scalar operands.  LLVM cannot fold these calls; Metal's compiler can (it knows its
// own intrinsics), so without this a `switch (min(N, 2))` with N set kept every case and marked
// buffers used that Metal's specialised code never reads (tests/fcrand.py).  Folding a call whose
// operands are all constants changes no value.
static Constant *foldAirCall(CallInst *CI, ArrayRef<Constant *> argv = {}) {
  Function *cf = CI->getCalledFunction();
  if (!cf || !cf->getName().starts_with("air.")) return nullptr;
  StringRef nm = cf->getName();
  Type *RT = CI->getType();
  if (RT->isVectorTy()) return nullptr;
  if (!argv.empty() && argv.size() != CI->arg_size()) return nullptr;
  SmallVector<ConstantInt *, 3> ci; SmallVector<ConstantFP *, 3> cfp;
  for (unsigned i = 0; i < CI->arg_size(); i++) {           // argv: the operands' values (static-initialiser evaluators)
    Value *a = argv.empty() ? argOperand(CI, i) : argv[i];
    if (auto *c = dyn_cast<ConstantInt>(a)) ci.push_back(c);
    else if (auto *f = dyn_cast<ConstantFP>(a)) cfp.push_back(f);
    else return nullptr;
  }
  auto I = [&](const APInt &v) { return ConstantInt::get(RT, v); };
  if (ci.size() == CI->arg_size() && RT->isIntegerTy()) {
    if (nm.starts_with("air.min.s.") && ci.size() == 2) return I(APIntOps::smin(ci[0]->getValue(), ci[1]->getValue()));
    if (nm.starts_with("air.max.s.") && ci.size() == 2) return I(APIntOps::smax(ci[0]->getValue(), ci[1]->getValue()));
    if (nm.starts_with("air.min.u.") && ci.size() == 2) return I(APIntOps::umin(ci[0]->getValue(), ci[1]->getValue()));
    if (nm.starts_with("air.max.u.") && ci.size() == 2) return I(APIntOps::umax(ci[0]->getValue(), ci[1]->getValue()));
    if (nm.starts_with("air.abs.s.") && ci.size() == 1) return I(ci[0]->getValue().abs());
    if (nm.starts_with("air.clamp.s.") && ci.size() == 3) return I(APIntOps::smin(APIntOps::smax(ci[0]->getValue(), ci[1]->getValue()), ci[2]->getValue()));
    if (nm.starts_with("air.clamp.u.") && ci.size() == 3) return I(APIntOps::umin(APIntOps::umax(ci[0]->getValue(), ci[1]->getValue()), ci[2]->getValue()));
  }
  if (nm.starts_with("air.convert.") && CI->arg_size() == 1) {
    SmallVector<StringRef, 6> parts; nm.split(parts, '.');
    if (parts.size() < 5) return nullptr;
    char dk = parts[2][0], sk = parts[4][0];
    Type *ST = argOperand(CI, 0)->getType();
    if (ST->isIntegerTy(1) || RT->isIntegerTy(1)) return nullptr;
    if (ci.size() == 1 && dk == 'f') {
      APFloat f(RT->getFltSemantics());
      f.convertFromAPInt(ci[0]->getValue(), sk == 's', APFloat::rmNearestTiesToEven);
      return ConstantFP::get(RT, f);
    }
    if (ci.size() == 1 && (dk == 's' || dk == 'u'))
      return I(sk == 's' ? ci[0]->getValue().sextOrTrunc(RT->getIntegerBitWidth()) : ci[0]->getValue().zextOrTrunc(RT->getIntegerBitWidth()));
    // verify6 must-fix 4: float -> integer truncates toward zero, as Metal folds it (and as
    // foldCtorAirCall does): char(-128.5) == -128, short(-32768.5) == -32768.  Left to NIR, the constant
    // reached nir_opcodes' f2i8 / f2i16 folding, which calls any source below the type's minimum
    // poison BEFORE truncating and returned 0 (cv_neg, cv_charm128, cv_shortm32768).  NaN or a value
    // whose truncation is out of range is poison in Metal: not folded.
    if (cfp.size() == 1 && (dk == 's' || dk == 'u') && RT->isIntegerTy()) {
      APSInt r(RT->getIntegerBitWidth(), dk == 'u'); bool exact = false;
      if (cfp[0]->getValueAPF().convertToInteger(r, APFloat::rmTowardZero, &exact) & APFloat::opInvalidOp) return nullptr;
      return ConstantInt::get(RT, r);
    }
  }
  return nullptr;
}
// AIR conversions in the static initialiser (fix round 5, verify5 item 2 and minor 1).  With a
// constant operand, the conversions the front end emits for bool(F), int(F), uint(F), short(H),
// float(N), half(N) ... fold, so a predicate that reads them is known when every constant is set, and
// judged by the value-independence model when some are unset.  v4 folded none of them: with every
// constant set a predicate such as `int(F) == 2` fell back to "present if used" (verify5 item 2), and
// with one unset the variant was refused whatever the call read (verify5 minor).  Only conversions:
// Metal itself rejects a pipeline whose static initialiser calls popcount / clz / clamp / abs / floor
// ("cannot have global constructors", tests/fixtures/v5/ctor_helpers.metal, regression test
// ctor_air_calls), so those still leave the initialiser unevaluated and the variant refused, as in
// Metal.  Metal's folding, measured (tests/fixtures/v5/convprobe.metal): float -> integer truncates
// toward zero (uint(-0.5) == 0); NaN or a truncated value outside the destination range is poison
// (int(F) == INT_MAX false for F = inf / 1e10 / 3e9, uint(-7.5) == 0 false, uchar(300.0) == 255
// false) and is NOT folded here: the variant is refused where it decides a predicate; bool(x) = x != 0
// (NaN true).
static Constant *foldCtorAirCall(CallInst *CI, ArrayRef<Constant *> args) {
  Function *cf = CI->getCalledFunction();
  if (!cf || !cf->getName().starts_with("air.convert.") || args.size() != 1 || CI->arg_size() != 1) return nullptr;
  StringRef nm = cf->getName();
  Type *RT = CI->getType();
  if (RT->isVectorTy()) return nullptr;
  SmallVector<StringRef, 6> parts; nm.split(parts, '.');
  if (parts.size() < 5) return nullptr;
  char dk = parts[2][0];
  if (auto *fx = dyn_cast<ConstantFP>(args[0])) {
    const APFloat &f = fx->getValueAPF();
    if (RT->isIntegerTy(1)) return ConstantInt::getBool(RT->getContext(), !f.isZero());
    if (RT->isIntegerTy() && (dk == 's' || dk == 'u')) {
      APSInt r(RT->getIntegerBitWidth(), dk == 'u'); bool exact = false;
      if (f.convertToInteger(r, APFloat::rmTowardZero, &exact) & APFloat::opInvalidOp) return nullptr;   // poison in Metal
      return ConstantInt::get(RT, r);
    }
    if (RT->isFloatingPointTy() && dk == 'f') {
      APFloat g = f; bool lost = false;
      g.convert(RT->getFltSemantics(), APFloat::rmNearestTiesToEven, &lost);
      return ConstantFP::get(RT, g);
    }
    return nullptr;
  }
  auto *ix = dyn_cast<ConstantInt>(args[0]);
  if (!ix) return nullptr;
  if (RT->isIntegerTy(1)) return ConstantInt::getBool(RT->getContext(), !ix->isZero());
  if (ix->getType()->isIntegerTy(1)) {
    if (RT->isFloatingPointTy()) return ConstantFP::get(RT, ix->isOne() ? 1.0 : 0.0);
    if (RT->isIntegerTy()) return ConstantInt::get(RT, ix->isOne() ? 1 : 0);
  }
  return foldAirCall(CI, args);                                   // integer -> float / integer
}
static std::string ctorCallStr(CallInst *CI, ArrayRef<Constant *> args) {
  std::string r; raw_string_ostream os(r);
  os << (CI->getCalledFunction() ? CI->getCalledFunction()->getName() : StringRef("?")) << "(";
  for (unsigned i = 0; i < args.size(); i++) { if (i) os << ", "; args[i]->printAsOperand(os, true); }
  os << ")";
  return os.str();
}
// An AIR integer min/max/abs/clamp or scalar conversion with an UNDEFINED operand is expanded into
// plain LLVM instructions (icmp + select, sitofp, ...), so resolve_unset_in_body can tell whether its
// result depends on the undefined value (`min(N, 2) >= -5` does not; `min(N, 2) == 2` does).
static Value *expandAirCall(CallInst *CI) {
  Function *cf = CI->getCalledFunction();
  if (!cf) return nullptr;
  StringRef nm = cf->getName();
  Type *RT = CI->getType();
  if (RT->isVectorTy()) return nullptr;
  IRBuilder<> B(CI);
  auto mm = [&](CmpInst::Predicate p, Value *a, Value *b) { return B.CreateSelect(B.CreateICmp(p, a, b), a, b); };
  if (RT->isIntegerTy() && !RT->isIntegerTy(1)) {
    if (nm.starts_with("air.min.s.")) return mm(CmpInst::ICMP_SLT, argOperand(CI, 0), argOperand(CI, 1));
    if (nm.starts_with("air.max.s.")) return mm(CmpInst::ICMP_SGT, argOperand(CI, 0), argOperand(CI, 1));
    if (nm.starts_with("air.min.u.")) return mm(CmpInst::ICMP_ULT, argOperand(CI, 0), argOperand(CI, 1));
    if (nm.starts_with("air.max.u.")) return mm(CmpInst::ICMP_UGT, argOperand(CI, 0), argOperand(CI, 1));
    if (nm.starts_with("air.clamp.s.")) return mm(CmpInst::ICMP_SLT, mm(CmpInst::ICMP_SGT, argOperand(CI, 0), argOperand(CI, 1)), argOperand(CI, 2));
    if (nm.starts_with("air.clamp.u.")) return mm(CmpInst::ICMP_ULT, mm(CmpInst::ICMP_UGT, argOperand(CI, 0), argOperand(CI, 1)), argOperand(CI, 2));
    if (nm.starts_with("air.abs.s.")) { Value *x = argOperand(CI, 0); return B.CreateSelect(B.CreateICmpSLT(x, Constant::getNullValue(RT)), B.CreateNeg(x), x); }
  }
  if (nm.starts_with("air.convert.") && CI->arg_size() == 1) {
    SmallVector<StringRef, 6> parts; nm.split(parts, '.');
    if (parts.size() < 5) return nullptr;
    char dk = parts[2][0], sk = parts[4][0];
    Value *x = argOperand(CI, 0); Type *ST = x->getType();
    if (ST->isIntegerTy(1) || RT->isIntegerTy(1) || ST->isVectorTy()) return nullptr;
    if (dk == 'f' && sk == 's') return B.CreateSIToFP(x, RT);
    if (dk == 'f' && sk == 'u') return B.CreateUIToFP(x, RT);
    if (dk == 's' && sk == 'f') return B.CreateFPToSI(x, RT);
    if (dk == 'u' && sk == 'f') return B.CreateFPToUI(x, RT);
    // integer -> integer: C semantics, the SOURCE signedness picks the extension (uint(short(-1)) is
    // 0xffffffff, int(ushort(65535)) is 65535); fix round 6 (v5 zero-extended every conversion to an
    // unsigned type and did not expand unsigned -> signed)
    if ((dk == 's' || dk == 'u') && sk == 's') return B.CreateSExtOrTrunc(x, RT);
    if ((dk == 's' || dk == 'u') && sk == 'u') return B.CreateZExtOrTrunc(x, RT);
  }
  return nullptr;
}
static bool foldAirCalls(Module &M) {
  bool any = false;
  for (Function &F : M) for (BasicBlock &BB : F) for (Instruction &I : make_early_inc_range(BB))
    if (auto *CI = dyn_cast<CallInst>(&I)) if (Constant *c = foldAirCall(CI)) { CI->replaceAllUsesWith(c); CI->eraseFromParent(); any = true; }
  return any;
}

struct UnsetResolveStats { unsigned markers = 0, folded = 0, independent = 0; };
static std::string fcNameOfGlobal(const GlobalVariable *G, const std::vector<FcDecl> &decls) {
  int idx = fcIndexOfGlobal(G->getName());
  for (auto &d : decls) if (d.index == idx || d.global == G->getName()) return d.name + " (index " + std::to_string(d.index) + ")";
  return G->getName().str();
}
// knownGlobals: every constant global the body may load whose value the specialisation decides:
// the function-constant initialisers (value, or nullptr when unset) and the static-initialiser
// shadows (the abstract evaluation's constant, or nullptr when undefined).
struct KnownGlobal { Constant *value = nullptr; std::string name; };
// ------------------------------------------ unset constants: value independence, proved (ABI v5) --
// verify4 item 1: ABI v4 decided "independent of the unset constant" from a handful of sample values
// (0, 1, 2, 3, -1, 5, 17, 255, INT_MIN, INT_MAX), so `N == 4` was false for every sample and folded to
// false, while Metal leaves it undefined and often takes the true side (verify4 work/fc/min m0..m6).
// ABI v5 folds a use of an unset constant only when the result is PROVABLY the same for every value
// the undefined operands can take, and every other use stays undefined (and is refused where it
// decides something).  The model is LLVM's undef as Metal's specialiser sees it: every USE of an
// unset constant may take a different value (so `N - N` or `A != A` is not independent), and a value
// that can be poison for some constant value is never folded.
//   - small domains are enumerated exhaustively: every operand combination whose domains multiply to
//     at most 2^16 (bool, i8 and i16 constants, i1 results, two i8s, ...), each combination folded by
//     LLVM (ConstantFold / InstSimplify), and the poison-generating flags (nsw/nuw/exact/disjoint/
//     nneg/samesign, fast-math nnan/ninf) and out-of-range shifts / zero divisors checked;
//   - wider integers (i32/i64) by LLVM ConstantRange arithmetic (its and/or/xor use KnownBits): an
//     icmp is independent only when ConstantRange::icmp proves it true (or its inverse true) for the
//     whole ranges, a binary operator or cast only when its range is a single value and no flag can
//     make it poison;
//   - floats and vectors: never independent through arithmetic (a float constant can be any value,
//     NaN and infinities included), only structurally (select with equal arms, extractelement of a
//     lane the constant never reached).
struct UAbs {                                   // an undefined (tainted) value
  bool poison = false;                          // may be poison for some value of the constants
  bool hasRange = false;                        // integer scalar: every value it may take is in cr
  ConstantRange cr = ConstantRange::getFull(1);
};
static UAbs uFull(Type *T) {
  UAbs a;
  if (T->isIntegerTy()) { a.hasRange = true; a.cr = ConstantRange::getFull(T->getIntegerBitWidth()); }
  return a;
}
static UAbs uJoin(const UAbs &x, const UAbs &y) {
  UAbs r; r.poison = x.poison || y.poison;
  if (x.hasRange && y.hasRange && x.cr.getBitWidth() == y.cr.getBitWidth()) { r.hasRange = true; r.cr = x.cr.unionWith(y.cr); }
  return r;
}
struct UOpnd { Value *v = nullptr; const UAbs *t = nullptr; };  // defined value (v), or undefined (t; v = the SSA value)
struct UResult { Value *v = nullptr; UAbs a; };                  // v: the result, the same for every value; else a
static const uint64_t UNSET_ENUM_LIMIT = 1u << 16;
static ConstantRange uRange(const UOpnd &o, unsigned w) {
  if (o.t) return o.t->hasRange && o.t->cr.getBitWidth() == w ? o.t->cr : ConstantRange::getFull(w);
  if (auto *ci = dyn_cast_or_null<ConstantInt>(o.v)) return ConstantRange(ci->getValue());
  return ConstantRange::getFull(w);
}
// Members of an undefined operand's domain, or empty when too many to enumerate.
static std::vector<Constant *> uMembers(const UOpnd &o, uint64_t limit) {
  std::vector<Constant *> r;
  Type *T = o.v->getType();
  if (!T->isIntegerTy() || o.t->poison) return r;
  unsigned w = T->getIntegerBitWidth();
  ConstantRange cr = uRange(o, w);
  if (cr.isEmptySet() || cr.isSizeLargerThan(limit)) return r;
  uint64_t n = cr.isFullSet() ? (uint64_t(1) << w) : (cr.getUpper() - cr.getLower()).getZExtValue();
  APInt x = cr.isFullSet() ? APInt::getZero(w) : cr.getLower();
  for (uint64_t i = 0; i < n; i++, ++x) r.push_back(ConstantInt::get(T->getContext(), x));
  return r;
}
// Does I, evaluated on these (constant where the constants were substituted) operands, give poison
// (or UB)?  `res` is what LLVM folded it to.  Conservative: true when it cannot tell.
static bool uPoisonAt(Instruction &I, ArrayRef<Value *> ops, Value *res) {
  if (auto *c = dyn_cast_or_null<Constant>(res)) if (isa<UndefValue>(c) || c->containsUndefOrPoisonElement()) return true;
  auto C = [&](unsigned i) -> ConstantInt * { return i < ops.size() ? dyn_cast<ConstantInt>(ops[i]) : nullptr; };
  auto FP = [&](Value *v) -> const ConstantFP * { return dyn_cast_or_null<ConstantFP>(v); };
  // fast-math nnan / ninf: poison when a substituted (constant) operand or the folded result is NaN /
  // infinite.  Non-constant operands are the variant's own run-time values, not the constant's.
  if (isa<FPMathOperator>(&I) && (I.hasNoNaNs() || I.hasNoInfs())) {
    auto bad = [&](Value *v) {
      if (!v || !v->getType()->isFPOrFPVectorTy() || !isa<Constant>(v)) return false;
      const ConstantFP *f = FP(v); if (!f) return true;   // constant vectors: cannot tell cheaply
      return (I.hasNoNaNs() && f->isNaN()) || (I.hasNoInfs() && f->isInfinity());
    };
    for (Value *v : ops) if (bad(v)) return true;
    if (bad(res)) return true;
  }
  unsigned opc = I.getOpcode();
  switch (opc) {
  case Instruction::Add: case Instruction::Sub: case Instruction::Mul: case Instruction::Shl: {
    auto *OBO = cast<OverflowingBinaryOperator>(&I);
    ConstantInt *a = C(0), *b = C(1);
    if (opc == Instruction::Shl && (!b || b->getValue().uge(b->getBitWidth()))) return true;
    if (!OBO->hasNoSignedWrap() && !OBO->hasNoUnsignedWrap()) return false;
    if (!a || !b) return true;
    bool so = false, uo = false; const APInt &x = a->getValue(), &y = b->getValue();
    if (opc == Instruction::Add) { (void)x.sadd_ov(y, so); (void)x.uadd_ov(y, uo); }
    if (opc == Instruction::Sub) { (void)x.ssub_ov(y, so); (void)x.usub_ov(y, uo); }
    if (opc == Instruction::Mul) { (void)x.smul_ov(y, so); (void)x.umul_ov(y, uo); }
    if (opc == Instruction::Shl) { (void)x.sshl_ov(y, so); (void)x.ushl_ov(y, uo); }
    return (OBO->hasNoSignedWrap() && so) || (OBO->hasNoUnsignedWrap() && uo);
  }
  case Instruction::LShr: case Instruction::AShr: {
    ConstantInt *a = C(0), *b = C(1);
    if (!b || b->getValue().uge(b->getBitWidth())) return true;
    if (cast<PossiblyExactOperator>(&I)->isExact()) return !a || a->getValue().countr_zero() < b->getValue().getZExtValue();
    return false;
  }
  case Instruction::UDiv: case Instruction::SDiv: case Instruction::URem: case Instruction::SRem: {
    ConstantInt *a = C(0), *b = C(1);
    if (!b || b->isZero()) return true;                                         // division by zero: UB
    if ((opc == Instruction::SDiv || opc == Instruction::SRem) && b->isMinusOne() && (!a || a->getValue().isMinSignedValue())) return true;
    if (isa<PossiblyExactOperator>(&I) && cast<PossiblyExactOperator>(&I)->isExact())
      return !a || !(opc == Instruction::UDiv ? a->getValue().urem(b->getValue()) : a->getValue().srem(b->getValue())).isZero();
    return false;
  }
  case Instruction::Or:
    if (cast<PossiblyDisjointInst>(&I)->isDisjoint()) { ConstantInt *a = C(0), *b = C(1); return !a || !b || (a->getValue() & b->getValue()) != 0; }
    return false;
  case Instruction::And: case Instruction::Xor: case Instruction::Select: case Instruction::PHI: case Instruction::Freeze:
  case Instruction::SExt: case Instruction::BitCast: case Instruction::FPExt: case Instruction::FPTrunc:
  case Instruction::SIToFP: case Instruction::FCmp: case Instruction::FAdd: case Instruction::FSub: case Instruction::FMul:
  case Instruction::FDiv: case Instruction::FRem: case Instruction::FNeg:
  case Instruction::ExtractElement: case Instruction::InsertElement: case Instruction::ShuffleVector:
  case Instruction::ExtractValue: case Instruction::InsertValue:
    return false;                                                               // (poison results caught above)
  case Instruction::ICmp:
    if (cast<ICmpInst>(&I)->hasSameSign()) { ConstantInt *a = C(0), *b = C(1); return !a || !b || a->isNegative() != b->isNegative(); }
    return false;
  case Instruction::Trunc: {
    auto *TI = cast<TruncInst>(&I);
    if (!TI->hasNoUnsignedWrap() && !TI->hasNoSignedWrap()) return false;
    ConstantInt *a = C(0); if (!a) return true;
    unsigned w = I.getType()->getScalarSizeInBits();
    return (TI->hasNoUnsignedWrap() && !a->getValue().isIntN(w)) || (TI->hasNoSignedWrap() && !a->getValue().isSignedIntN(w));
  }
  case Instruction::ZExt: case Instruction::UIToFP:
    if (cast<PossiblyNonNegInst>(&I)->hasNonNeg()) { ConstantInt *a = C(0); return !a || a->isNegative(); }
    return false;
  default:
    return canCreateUndefOrPoison(cast<Operator>(&I), /*ConsiderFlagsAndMetadata=*/true);
  }
}
// Evaluate I, some of whose operands are undefined, under the model above.
//   isUndef(v): whether the value v (a folded result) is itself undefined.
static UResult uEval(Instruction &I, ArrayRef<UOpnd> ops, const SimplifyQuery &SQ, const TargetLibraryInfo *TLI,
                     const std::function<bool(Value *)> &isUndef) {
  const DataLayout &DL = I.getModule()->getDataLayout();
  UResult R;
  bool anyPoison = false;
  for (auto &o : ops) if (o.t && o.t->poison) anyPoison = true;
  auto undefOut = [&](bool poison) { UResult u; u.a = uFull(I.getType()); u.a.poison = poison; return u; };
  // select on a defined constant condition: the chosen arm, whatever the other one is
  if (auto *Sel = dyn_cast<SelectInst>(&I))
    if (!ops[0].t) if (auto *c = dyn_cast<ConstantInt>(ops[0].v)) { R.v = c->isOne() ? Sel->getTrueValue() : Sel->getFalseValue(); return R; }
  // structural: a lane / field the undefined value never reached (no arithmetic on it)
  if (isa<ExtractElementInst>(&I) || isa<ExtractValueInst>(&I) || isa<ShuffleVectorInst>(&I)) {
    if (Value *r = simplifyInstruction(&I, SQ.getWithInstruction(&I))) { R.v = r; return R; }
  }
  // 1. exhaustive enumeration of small domains
  std::vector<unsigned> tops; std::vector<std::vector<Constant *>> dom; uint64_t combos = 1; bool enumerable = true;
  for (unsigned i = 0; i < ops.size(); i++) {
    if (!ops[i].t) continue;
    auto m = uMembers(ops[i], UNSET_ENUM_LIMIT);
    if (m.empty()) { enumerable = false; break; }
    combos *= m.size();
    if (combos > UNSET_ENUM_LIMIT) { enumerable = false; break; }
    tops.push_back(i); dom.push_back(std::move(m));
  }
  if (enumerable && !tops.empty()) {
    Value *common = nullptr; bool same = true, poison = false, allInt = true;
    APInt lo, hi; bool haveLH = false;
    SmallVector<Value *, 4> vals; for (auto &o : ops) vals.push_back(o.v);
    for (uint64_t c = 0; c < combos; c++) {
      uint64_t k = c;
      for (size_t j = 0; j < tops.size(); j++) { vals[tops[j]] = dom[j][k % dom[j].size()]; k /= dom[j].size(); }
      Value *r = nullptr;
      bool allC = !isa<PHINode>(&I);
      SmallVector<Constant *, 4> cs;
      for (Value *v : vals) { auto *cc = dyn_cast<Constant>(v); if (!cc || isa<GlobalValue>(cc)) { allC = false; break; } cs.push_back(cc); }
      if (allC) r = ConstantFoldInstOperands(&I, cs, DL, TLI);
      if (!r) r = simplifyInstructionWithOperands(&I, vals, SQ.getWithInstruction(&I));
      if (!r) return undefOut(anyPoison);                                       // not foldable: depends on it
      if (uPoisonAt(I, vals, r)) { poison = true; same = false; break; }
      if (!common) common = r; else if (common != r) same = false;
      if (auto *ci = dyn_cast<ConstantInt>(r)) {
        if (!haveLH) { lo = hi = ci->getValue(); haveLH = true; }
        else { if (ci->getValue().ult(lo)) lo = ci->getValue(); if (ci->getValue().ugt(hi)) hi = ci->getValue(); }
      } else allInt = false;
      if (!same && !allInt) break;
    }
    if (same && !poison && !anyPoison) { R.v = common; return R; }
    UResult u = undefOut(poison || anyPoison);
    if (!poison && allInt && haveLH && I.getType()->isIntegerTy()) { u.a.hasRange = true; u.a.cr = ConstantRange::getNonEmpty(lo, hi + 1); }
    return u;
  }
  // 2. integer ranges
  Type *RT = I.getType();
  auto isIntOp = [&](const UOpnd &o) { return o.v->getType()->isIntegerTy(); };
  bool ints = RT->isIntegerTy();
  for (auto &o : ops) if (!isIntOp(o)) ints = false;
  if (ints && (isa<BinaryOperator>(&I) || isa<ICmpInst>(&I) || isa<TruncInst>(&I) || isa<ZExtInst>(&I) || isa<SExtInst>(&I) ||
               isa<SelectInst>(&I) || isa<PHINode>(&I) || isa<FreezeInst>(&I))) {
    unsigned w = RT->getIntegerBitWidth();
    UResult u = undefOut(anyPoison);
    bool poison = anyPoison;
    if (auto *BO = dyn_cast<BinaryOperator>(&I)) {
      ConstantRange L = uRange(ops[0], w), Rr = uRange(ops[1], w);
      unsigned opc = BO->getOpcode();
      using OR = ConstantRange::OverflowResult;
      if (auto *OBO = dyn_cast<OverflowingBinaryOperator>(BO)) {
        bool nsw = OBO->hasNoSignedWrap(), nuw = OBO->hasNoUnsignedWrap();
        if (opc == Instruction::Add) { if (nsw && L.signedAddMayOverflow(Rr) != OR::NeverOverflows) poison = true; if (nuw && L.unsignedAddMayOverflow(Rr) != OR::NeverOverflows) poison = true; }
        if (opc == Instruction::Sub) { if (nsw && L.signedSubMayOverflow(Rr) != OR::NeverOverflows) poison = true; if (nuw && L.unsignedSubMayOverflow(Rr) != OR::NeverOverflows) poison = true; }
        if (opc == Instruction::Mul) {
          if (nsw) {                                            // no signedMulMayOverflow: multiply at twice the width
            ConstantRange P = L.signExtend(2 * w).multiply(Rr.signExtend(2 * w));
            if (P.getSignedMin().slt(APInt::getSignedMinValue(w).sext(2 * w)) || P.getSignedMax().sgt(APInt::getSignedMaxValue(w).sext(2 * w))) poison = true;
          }
          if (nuw && L.unsignedMulMayOverflow(Rr) != OR::NeverOverflows) poison = true;
        }
        if (opc == Instruction::Shl && (nsw || nuw)) poison = true;
      }
      if ((opc == Instruction::Shl || opc == Instruction::LShr || opc == Instruction::AShr) && Rr.getUnsignedMax().uge(w)) poison = true;
      if (isa<PossiblyExactOperator>(BO) && BO->isExact()) poison = true;
      if (opc == Instruction::UDiv || opc == Instruction::SDiv || opc == Instruction::URem || opc == Instruction::SRem) {
        if (Rr.contains(APInt::getZero(w))) poison = true;
        if ((opc == Instruction::SDiv || opc == Instruction::SRem) && L.contains(APInt::getSignedMinValue(w)) && Rr.contains(APInt::getAllOnes(w))) poison = true;
      }
      if (opc == Instruction::Or && cast<PossiblyDisjointInst>(BO)->isDisjoint()) poison = true;
      u.a.cr = L.binaryOp((Instruction::BinaryOps)opc, Rr);
    } else if (auto *IC = dyn_cast<ICmpInst>(&I)) {
      unsigned ow = ops[0].v->getType()->getIntegerBitWidth();
      ConstantRange L = uRange(ops[0], ow), Rr = uRange(ops[1], ow);
      if (IC->hasSameSign()) poison = true;
      if (L.icmp(IC->getPredicate(), Rr)) u.a.cr = ConstantRange(APInt(1, 1));
      else if (L.icmp(IC->getInversePredicate(), Rr)) u.a.cr = ConstantRange(APInt(1, 0));
      else u.a.cr = ConstantRange::getFull(1);
    } else if (auto *TI = dyn_cast<TruncInst>(&I)) {
      if (TI->hasNoUnsignedWrap() || TI->hasNoSignedWrap()) poison = true;
      u.a.cr = uRange(ops[0], ops[0].v->getType()->getIntegerBitWidth()).truncate(w);
    } else if (auto *ZI = dyn_cast<ZExtInst>(&I)) {
      ConstantRange L = uRange(ops[0], ops[0].v->getType()->getIntegerBitWidth());
      if (ZI->hasNonNeg() && !L.isAllNonNegative()) poison = true;
      u.a.cr = L.zeroExtend(w);
    } else if (isa<SExtInst>(&I)) {
      u.a.cr = uRange(ops[0], ops[0].v->getType()->getIntegerBitWidth()).signExtend(w);
    } else if (auto *Sel = dyn_cast<SelectInst>(&I)) {
      if (Sel->getTrueValue() == Sel->getFalseValue()) { R.v = Sel->getTrueValue(); if (!ops[0].t || !ops[0].t->poison) return R; }
      u.a.cr = uRange(ops[1], w).unionWith(uRange(ops[2], w));
    } else if (auto *PN = dyn_cast<PHINode>(&I)) {
      if (Value *same = PN->hasConstantValue()) { R.v = same; if (!anyPoison) return R; }
      ConstantRange cr = ConstantRange::getEmpty(w);
      for (auto &o : ops) cr = cr.unionWith(uRange(o, w));
      u.a.cr = cr;
    } else if (isa<FreezeInst>(&I)) {
      u.a.cr = uRange(ops[0], w); poison = false;
    }
    u.a.hasRange = true; u.a.poison = poison;
    if (!poison && u.a.cr.isSingleElement()) { R.v = ConstantInt::get(RT, *u.a.cr.getSingleElement()); return R; }
    return u;
  }
  // 3. anything else: select with equal arms, otherwise undefined
  if (auto *Sel = dyn_cast<SelectInst>(&I))
    if (Sel->getTrueValue() == Sel->getFalseValue() && !(ops[0].t && ops[0].t->poison)) { R.v = Sel->getTrueValue(); return R; }
  if (auto *PN = dyn_cast<PHINode>(&I))
    if (Value *same = PN->hasConstantValue()) if (!anyPoison) { R.v = same; return R; }
  return undefOut(anyPoison || canCreateUndefOrPoison(cast<Operator>(&I), /*ConsiderFlagsAndMetadata=*/true));
}

// resolve_unset_in_body (ABI v4, verify3 item 1; proof rule ABI v5, verify4 item 1): an unset
// function constant must not decide anything in the function body.  Its loads become "undefined"
// markers (any value of the constant's type); every instruction that uses one is evaluated by uEval:
// if the result is provably the same for every value (exhaustively for small domains, by
// ConstantRange otherwise), the constant does not matter there and the instruction folds to that
// result (`A && B` with B = false set, `and x, 0`, `select u, x, x`, `(N & 3) < 4`); otherwise the
// result stays undefined, with the range of values it may take.  A variant in which an undefined
// value decides a branch, a switch, a select that is used, a store, a call or the returned value is
// REFUSED:
//   "the value of unset function constant X reaches the body ...: set the constant".
// ABI v4 decided independence from ten sample values and folded `N == 4` to false (verify4 item 1).
// Why not reproduce Metal's choice, as the static initialiser does for the predicates?  Metal
// specialises with LLVM `undef` and its optimiser's choices depend on the surrounding code: the
// randomized differential test (tests/fcrand.py) and the reductions in scratch/probe found two
// branches of the same shape going opposite ways, `select u, true, false` giving false in one
// function and true in another, and no rule that matches: SCCP's false edge (ABI v3 fix round 2)
// matched 826 of 960 cases, JumpThreading's fewest-predecessors rule 772, Apple's own air-opt
// O1..O3 disagreed as well.  With this refusal every compiled variant matches Metal.
static UnsetResolveStats resolve_unset_in_body(Module &M, Function *entry, const std::set<Function *> &ctorFns,
                                               const std::map<const GlobalVariable *, KnownGlobal> &knownGlobals,
                                               const std::map<int, std::vector<double>> &fc) {
  UnsetResolveStats S;
  const DataLayout &DL = M.getDataLayout();
  // only the entry point and what it calls (a .air may hold other entry points: not ours to judge)
  std::set<Function *> reach{entry};
  { SmallVector<Function *, 8> w{entry};
    while (!w.empty()) { Function *f = w.pop_back_val();
      for (BasicBlock &BB : *f) for (Instruction &I : BB) if (auto *CI = dyn_cast<CallInst>(&I))
        if (Function *c = CI->getCalledFunction()) if (!c->isDeclaration() && reach.insert(c).second) w.push_back(c); } }
  std::map<Value *, std::string> origin;                        // marker -> which constant (for the refusal message)
  std::set<Value *> T;                                          // undefined values (markers + derived)
  std::map<Value *, UAbs> tabs;                                 // what each undefined value may be (uEval)
  std::vector<WeakVH> Tvh; std::vector<std::pair<WeakVH, std::string>> originVh;
  auto taint = [&](Value *v) { if (T.insert(v).second) Tvh.push_back(WeakVH(v)); };
  auto refreshT = [&] {
    T.clear(); std::vector<WeakVH> keep;
    for (auto &h : Tvh) if (h) { T.insert(h); keep.push_back(h); }
    Tvh.swap(keep);
    origin.clear(); for (auto &o : originVh) if (o.first) origin[(Value *)o.first] = o.second;
  };
  std::set<Function *> fns;
  for (auto &kv : knownGlobals) {
    GlobalVariable *G = const_cast<GlobalVariable *>(kv.first);
    SmallVector<User *, 16> work(G->user_begin(), G->user_end());
    while (!work.empty()) {
      User *U = work.pop_back_val();
      if (auto *CE = dyn_cast<ConstantExpr>(U)) { for (User *u2 : CE->users()) work.push_back(u2); continue; }
      auto *I = dyn_cast<Instruction>(U);
      if (!I || ctorFns.count(I->getFunction()) || !reach.count(I->getFunction())) continue;
      if (auto *LI = dyn_cast<LoadInst>(I)) {
        Constant *v = kv.second.value;
        bool direct = LI->getPointerOperand()->stripPointerCasts() == G && v && v->getType() == LI->getType();
        if (v && !direct) continue;                             // partial load of a defined constant: GlobalOpt folds it later
        fns.insert(LI->getFunction());
        if (v) { LI->replaceAllUsesWith(v); LI->eraseFromParent(); continue; }
        auto *mk = new FreezeInst(PoisonValue::get(LI->getType()), "air2ir3.unset", LI->getIterator());
        LI->replaceAllUsesWith(mk); LI->eraseFromParent();
        tabs[mk] = uFull(mk->getType()); taint(mk); origin[mk] = kv.second.name; originVh.push_back({WeakVH(mk), kv.second.name}); S.markers++;
        continue;
      }
      if (auto *GEP = dyn_cast<GetElementPtrInst>(I)) { for (User *u2 : GEP->users()) work.push_back(u2); continue; }
      if (auto *BC = dyn_cast<CastInst>(I)) { for (User *u2 : BC->users()) work.push_back(u2); continue; }
      if (auto *CI = dyn_cast<CallInst>(I))                     // is_function_constant_defined(@c): folded below
        if (CI->getCalledFunction() && CI->getCalledFunction()->getName() == "air.is_function_constant_defined") continue;
      if (!kv.second.value) die("unset function constant " + kv.second.name + " is used by address (" + str(I) + "): set it");
    }
  }
  if (T.empty()) return S;                                      // nothing undefined reaches the body
  // is_function_constant_defined() is known up front.
  for (Function *F : fns) for (BasicBlock &BB : *F) for (Instruction &I : make_early_inc_range(BB))
    if (auto *CI = dyn_cast<CallInst>(&I))
      if (CI->getCalledFunction() && CI->getCalledFunction()->getName() == "air.is_function_constant_defined") {
        auto *G = dyn_cast<GlobalVariable>(argOperand(CI, 0)->stripPointerCasts());
        int idx = G ? fcIndexOfGlobal(G->getName()) : -1;
        CI->replaceAllUsesWith(ConstantInt::getBool(CI->getContext(), idx >= 0 && fc.count(idx))); CI->eraseFromParent();
      }
  auto originOf = [&](Value *v0) {
    std::set<Value *> seen; SmallVector<Value *, 8> w{v0};
    while (!w.empty()) {
      Value *x = w.pop_back_val();
      if (!seen.insert(x).second) continue;
      auto o = origin.find(x); if (o != origin.end()) return o->second;
      if (auto *xi = dyn_cast<Instruction>(x)) for (Value *op : xi->operands()) if (T.count(op)) w.push_back(op);
    }
    return std::string("?");
  };
  auto refuse = [&](Value *v, Instruction *where, const char *what) {
    std::string w = str(where); for (char &ch : w) if (ch == '\n') ch = ' ';
    die("the value of unset function constant " + originOf(v) + " " + what + " in the function body (" + w.substr(0, 140) +
        "); Metal leaves it undefined and its choice is not reproducible: set the constant (BINDING-ABI.md 3.7)");
  };
  TargetLibraryInfoImpl TLII(M.getTargetTriple()); TargetLibraryInfo TLI(TLII);
  // The value-independence evaluation of I (uEval) with the ranges of its undefined operands.
  auto evalUndef = [&](Instruction &I, const SimplifyQuery &SQ) -> UResult {
    SmallVector<UOpnd, 4> ops;
    for (Value *op : I.operands()) {
      UOpnd o; o.v = op;
      if (T.count(op)) { auto it = tabs.find(op); if (it == tabs.end()) it = tabs.emplace(op, uFull(op->getType())).first; o.t = &it->second; }
      ops.push_back(o);
    }
    return uEval(I, ops, SQ, &TLI, [&](Value *v) { return T.count(v) != 0; });
  };
  for (Function *F : fns) {
    bool changed = true;
    for (int iter = 0; changed; iter++) {
      if (iter > 10000) die("internal: resolving unset constants did not converge");
      changed = false;
      bool cfgChanged = false;
      DominatorTree DT(*F);
      SimplifyQuery SQ(DL, &TLI, &DT);
      ReversePostOrderTraversal<Function *> RPOT(F);
      for (BasicBlock *BB : RPOT) {
        for (Instruction &I : make_early_inc_range(*BB)) {
          if (T.count(&I)) continue;                           // already undefined (monotone)
          bool anyT = false, anyC = false, allC = true;
          for (Value *op : I.operands()) {
            if (T.count(op)) anyT = true;
            if (isa<Constant>(op) && !isa<GlobalValue>(op)) anyC = true; else if (!isa<BasicBlock>(op)) allC = false;
          }
          if (auto *Br = dyn_cast<BranchInst>(&I)) {
            if (!Br->isConditional()) continue;
            Value *c = Br->getCondition();
            if (T.count(c)) refuse(c, Br, "decides a branch");
            auto *ci = dyn_cast<ConstantInt>(c);
            if (!ci) continue;
            BasicBlock *keep = Br->getSuccessor(ci->isZero() ? 1 : 0), *drop = Br->getSuccessor(ci->isZero() ? 0 : 1);
            if (drop != keep) drop->removePredecessor(BB, /*KeepOneInputPHIs=*/true);
            BranchInst::Create(keep, Br->getIterator()); Br->eraseFromParent();
            changed = cfgChanged = true;
            break;
          }
          if (auto *SW = dyn_cast<SwitchInst>(&I)) {
            Value *c = SW->getCondition();
            if (T.count(c)) refuse(c, SW, "decides a switch");
            auto *ci = dyn_cast<ConstantInt>(c);
            if (!ci) continue;
            BasicBlock *keep = SW->findCaseValue(ci)->getCaseSuccessor();
            std::set<BasicBlock *> seen;
            for (unsigned i = 0; i < SW->getNumSuccessors(); i++) {
              BasicBlock *s = SW->getSuccessor(i);
              if (s != keep && seen.insert(s).second) s->removePredecessor(BB, /*KeepOneInputPHIs=*/true);
            }
            BranchInst::Create(keep, SW->getIterator()); SW->eraseFromParent();
            changed = cfgChanged = true;
            break;
          }
          bool pure = !I.getType()->isVoidTy() && !I.isTerminator() && !I.mayHaveSideEffects();
          if (!anyT) {
            // propagate the defined values first (Metal's GlobalOpt+InstCombine do), constant results only
            if (!(pure || (isa<CallInst>(&I) && allC && foldAirCall(cast<CallInst>(&I)))) || !(anyC || isa<PHINode>(&I))) continue;
            Value *r = allC && !isa<PHINode>(&I) ? ConstantFoldInstruction(&I, DL, &TLI) : simplifyInstruction(&I, SQ.getWithInstruction(&I));
            if (!r && allC) if (auto *CI = dyn_cast<CallInst>(&I)) r = foldAirCall(CI);
            if (!r) continue;
            auto *rc = dyn_cast<Constant>(r);
            if (!isa<PHINode>(&I) && !rc) continue;
            if (rc && (isa<UndefValue>(rc) || rc->containsUndefOrPoisonElement())) continue;
            I.replaceAllUsesWith(r); I.eraseFromParent(); changed = true;
            continue;
          }
          if (auto *CI = dyn_cast<CallInst>(&I))                // AIR integer ops / conversions -> plain instructions
            if (Value *e = expandAirCall(CI)) { CI->replaceAllUsesWith(e); CI->eraseFromParent(); changed = true; continue; }
          if (!pure) refuse(&I, &I, isa<ReturnInst>(&I) ? "reaches the returned value" : "reaches a store or a call");
          UResult ur = evalUndef(I, SQ);
          if (ur.v && ur.v != &I) {
            I.replaceAllUsesWith(ur.v); I.eraseFromParent(); S.independent++; changed = true;
            continue;
          }
          tabs[&I] = ur.a; taint(&I); changed = true;
        }
        if (cfgChanged) break;                                 // the traversal and DT are stale
      }
      if (cfgChanged) {
        std::set<BasicBlock *> alive;
        for (BasicBlock *bb : depth_first(&F->getEntryBlock())) alive.insert(bb);
        for (auto it = T.begin(); it != T.end();) {
          auto *I = cast<Instruction>(*it);
          if (I->getFunction() == F && !alive.count(I->getParent())) it = T.erase(it); else ++it;
        }
        removeUnreachableBlocks(*F);
      }
      refreshT();
    }
  }
  refreshT();
  // Anything still undefined must be dead: a use by a defined instruction means the constant
  // decides something the variant computes.
  std::set<Instruction *> live;
  for (Value *v : T) live.insert(cast<Instruction>(v));
  for (Instruction *I : live)
    for (User *U : I->users())
      if (auto *UI = dyn_cast<Instruction>(U))
        if (!T.count(UI)) refuse(I, UI, "reaches");
  bool again = true;
  while (again) {
    again = false;
    for (auto it = live.begin(); it != live.end();) {
      if ((*it)->use_empty()) { (*it)->eraseFromParent(); it = live.erase(it); again = true; } else ++it;
    }
  }
  if (!live.empty()) die("internal: undefined values left after resolving unset constants");
  return S;
}

// Which unset function constants each static-initialiser value can depend on (data and control
// dependence, over-approximated: a store below a conditional branch depends on the branch's
// condition).  Used to check whether a predicate is in fact the same for every value of the
// constants it reads (e.g. `(C && !B) || (!C && is_function_constant_defined(B))` with B set and
// C unset is true for C = 0 and C = 1, while the undef rules say undefined -> absent).
static std::map<const Value *, std::set<int>> ctorDeps(Module &M, const std::map<int, std::vector<double>> &fc) {
  std::map<const Value *, std::set<int>> dep;
  GlobalVariable *ctors = M.getGlobalVariable("llvm.global_ctors");
  if (!ctors || !ctors->hasInitializer()) return dep;
  auto *CA = dyn_cast<ConstantArray>(ctors->getInitializer());
  if (!CA) return dep;
  for (int round = 0; round < 8; round++) {
    bool changed = false;
    for (Use &U : CA->operands()) {
      auto *CS = dyn_cast<ConstantStruct>(U.get()); if (!CS) continue;
      auto *Fn = dyn_cast<Function>(CS->getOperand(1)->stripPointerCasts()); if (!Fn || Fn->isDeclaration()) continue;
      std::set<int> ctrl;
      for (BasicBlock &BB : *Fn) if (auto *Br = dyn_cast<BranchInst>(BB.getTerminator())) if (Br->isConditional())
        { auto it = dep.find(Br->getCondition()); if (it != dep.end()) ctrl.insert(it->second.begin(), it->second.end()); }
      for (BasicBlock &BB : *Fn) if (auto *SW = dyn_cast<SwitchInst>(BB.getTerminator()))
        { auto it = dep.find(SW->getCondition()); if (it != dep.end()) ctrl.insert(it->second.begin(), it->second.end()); }
      for (BasicBlock &BB : *Fn) for (Instruction &I : BB) {
        std::set<int> d;
        if (auto *LI = dyn_cast<LoadInst>(&I)) {
          if (auto *G = dyn_cast<GlobalVariable>(LI->getPointerOperand()->stripPointerCasts())) {
            if (G->getSection() == "air.fc_initializer") { int idx = fcIndexOfGlobal(G->getName()); if (idx >= 0 && !fc.count(idx)) d.insert(idx); }
            else { auto it = dep.find(G); if (it != dep.end()) d = it->second; }
          }
        } else for (Value *op : I.operands()) { auto it = dep.find(op); if (it != dep.end()) d.insert(it->second.begin(), it->second.end()); }
        if (isa<PHINode>(&I) || isa<StoreInst>(&I)) d.insert(ctrl.begin(), ctrl.end());
        if (auto *SI = dyn_cast<StoreInst>(&I)) {
          if (auto *G = dyn_cast<GlobalVariable>(SI->getPointerOperand()->stripPointerCasts())) {
            auto &g = dep[G]; size_t n = g.size(); g.insert(d.begin(), d.end()); if (g.size() != n) changed = true;
          }
          continue;
        }
        auto &cur = dep[&I]; size_t n = cur.size(); cur.insert(d.begin(), d.end()); if (cur.size() != n) changed = true;
      }
    }
    if (!changed) break;
  }
  return dep;
}
