// SPDX-License-Identifier: MIT
// Function-constant value canonicalisation, the LLVM pass pipelines and the abstract evaluation of the
// static initialiser (llvm.global_ctors).
// Part of src/air2ir3.cpp's translation unit, #included from it in order; not compiled on its own.

// ----------------------------------------------------- LLVM-level preparation --
// 1) function-constant specialisation, 2) evaluate the static initialiser (function-constant
//    predicates and attribute-index expressions), 3) inline everything into the entry,
// 4) cleanup pipeline so the translator sees small SSA code.
// Function-constant values in their DECLARED type (cache format 5, ABI 6; verify6 must-fix 2).  The
// CLI and the C API carry values as doubles; resolveFcs canonicalises every component once, here, to
// the value the declared type holds (integers wrapped to their width, bool 0/1, float / half rounded
// to that format, NaN / inf / -0 kept), and pads a vector to its component count.  The compile, the
// key bytes (fcBytes) and the sidecar (fcValueJson) all read that one canonical value, so the key
// recomputes from the sidecar for every value.  long / ulong beyond 2^53 cannot arrive through a
// double (C API 2 limit); what the double holds is canonicalised the same way everywhere.
static std::string fcScalarBase(const std::string &t, unsigned &n) {
  static const char *bases[] = {"uchar", "char", "ushort", "short", "uint", "int", "ulong", "long", "half", "float", "bool"};
  for (const char *b : bases)
    if (t.rfind(b, 0) == 0) { std::string r = t.substr(strlen(b)); n = r.empty() ? 1 : atoi(r.c_str()); return b; }
  die("function-constant type " + t);
}
static int64_t fcToI64(double v) {                  // C conversion toward zero, wrapping modulo 2^64 (no UB)
  if (std::isnan(v)) return 0;
  if (v >= 9223372036854775808.0) return v >= 18446744073709551616.0 ? -1 : (int64_t)(uint64_t)v;
  if (v < -9223372036854775808.0) return INT64_MIN;
  return (int64_t)v;
}
static double fcCanonScalar(const std::string &base, double v) {
  int64_t i = fcToI64(v);
  if (base == "bool") return v != 0 ? 1 : 0;               // NaN != 0: true
  if (base == "uchar") return (double)(uint8_t)i;
  if (base == "char") return (double)(int8_t)i;
  if (base == "ushort") return (double)(uint16_t)i;
  if (base == "short") return (double)(int16_t)i;
  if (base == "uint") return (double)(uint32_t)i;
  if (base == "int") return (double)(int32_t)i;
  if (base == "ulong") return (double)(uint64_t)i;
  if (base == "long") return (double)i;
  if (base == "float") return (double)(float)v;
  if (base == "half") return (double)(_Float16)v;
  die("function-constant type " + base);
}
static std::vector<double> fcCanon(const std::string &type, const std::vector<double> &v) {
  unsigned n = 1; std::string base = fcScalarBase(type, n);
  std::vector<double> r;
  for (unsigned i = 0; i < n; i++) r.push_back(fcCanonScalar(base, i < v.size() ? v[i] : (v.empty() ? 0 : v[0])));
  return r;
}
static uint64_t fcIntBits(double v) { return v < 0 ? (uint64_t)(int64_t)v : (uint64_t)v; }   // a canonical integer value
static Constant *fcConstant(Type *T, const std::vector<double> &v) {
  auto scalar = [&](Type *E, double x) -> Constant * {
    if (E->isFloatingPointTy()) return ConstantFP::get(E, x);
    return ConstantInt::get(E, fcIntBits(x));
  };
  if (auto *VT = dyn_cast<FixedVectorType>(T)) {
    std::vector<Constant *> el;
    for (unsigned i = 0; i < VT->getNumElements(); i++) el.push_back(scalar(VT->getElementType(), v.size() > i ? v[i] : (v.empty() ? 0 : v[0])));
    return ConstantVector::get(el);
  }
  return scalar(T, v.empty() ? 0 : v[0]);
}
// A load of a function-constant initialiser global (fix round 5, verify5 item 3): the value is built
// in the constant's DECLARED type (the global's value type) and its bytes are reinterpreted as the
// load's type.  The front end loads a float / half constant as an integer of the same size for
// isnan / isinf / signbit / as_type (`load i32, ptr @_Z1F.MTL_FC_INIT_4_f` in a static initialiser);
// converting the VALUE to the load's type (fix round 4 and before) turned F = -7.5 into the integer
// -7, whose bit pattern isnan() reads as a NaN (pun_ctor_isnan: buffer(2) present, Metal absent).
// Used by abstract_eval_ctors, indep_eval_ctors and the body replacement in specialize_and_inline.
static Constant *fcLoadConstant(const GlobalVariable *G, Type *loadT, const std::vector<double> &v, const DataLayout &DL) {
  Type *DT = G->getValueType();
  if (DT == loadT) return fcConstant(loadT, v);
  if (Constant *c = ConstantFoldLoadFromConst(fcConstant(DT, v), loadT, DL)) return c;
  std::string t; raw_string_ostream os(t); loadT->print(os); DT->print(os << " from ");
  die("function constant " + G->getName().str() + " is loaded as " + os.str() + ": cannot reinterpret its bytes");
}
// A static-initialiser load of a global the initialiser stored with another type (a shadow read
// through as_type): the stored constant's bytes, as a load from memory gives them (fix round 5).
static Constant *punStored(Constant *c, Type *loadT, const DataLayout &DL) {
  return c->getType() == loadT ? c : ConstantFoldLoadFromConst(c, loadT, DL);
}

// PassBuilder pipelines. All instcombine instances use <no-verify-fixpoint>: LLVM >= 18 aborts
// ("did not reach a fixpoint after 1 iterations") on some specialised WindowServer variants
// (39 of 410 SkyLight function-constant variants).  Not reaching the fixpoint is harmless here.
static const char *PIPE_PRE = "function(instcombine<no-verify-fixpoint>),globalopt";
// PIPE_MAIN folds the (now constant) function constants and prunes the dead feature branches and helpers before it
// inlines: inlining the ubershader's every call first and folding afterwards was 68% of a specialised CoreAnimation
// compile, for the same code (docs/USERSPACE-GRAPHICS-20261019.md, "Speed: shader compile time").
static const char *PIPE_MAIN =
    "function(instcombine<no-verify-fixpoint>,simplifycfg),globaldce,always-inline,globaldce,"
    "function(sroa,early-cse<memssa>,instcombine<no-verify-fixpoint>,gvn,simplifycfg,"
    "instcombine<no-verify-fixpoint>,adce,simplifycfg)";
static const char *PIPE_POST = "function(instcombine<no-verify-fixpoint>,simplifycfg,adce,simplifycfg)";

static void runPipeline(Module &M, const char *pipe) {
  LoopAnalysisManager LAM; FunctionAnalysisManager FAM; CGSCCAnalysisManager CGAM; ModuleAnalysisManager MAM;
  PassBuilder PB;
  PB.registerModuleAnalyses(MAM); PB.registerCGSCCAnalyses(CGAM); PB.registerFunctionAnalyses(FAM);
  PB.registerLoopAnalyses(LAM); PB.crossRegisterProxies(LAM, FAM, CGAM, MAM);
  ModulePassManager MPM;
  if (auto Err = PB.parsePassPipeline(MPM, pipe)) die(toString(std::move(Err)));
  MPM.run(M, MAM);
}

struct SpecResult {
  std::map<std::string, int64_t> implicitVals;   // evaluated static-initialiser globals (by name)
  std::map<int, bool> fcDefined;
  std::set<std::string> taintedGlobals;          // globals whose value depends on an unset function constant
  unsigned taintedPredicates = 0;                // air.normalize_function_constant_predicate calls forced to 0
  bool ctorEvaluated = false;
  unsigned unsetMarkers = 0, unsetIndependent = 0;           // resolve_unset_in_body
  std::map<std::string, std::string> ambiguousPredicates;    // presenceVerdicts: refused if they gate something
  std::set<std::string> directPredicates;                    // presenceVerdicts: direct copies of an unset constant (absent)
  std::string ctorWhy;                                       // why the static initialiser could not be evaluated (if so)
};

// Unset function constants, Metal semantics (BINDING-ABI.md 3.7).  Metal leaves a constant that
// the pipeline does not set undefined: is_function_constant_defined() is false, and an argument or
// output gated by a predicate that evaluates to "undefined" is ABSENT.  Measured on the M3 Max
// (tests/regress.py "unset_fc_semantics", tests/fixtures/fcsem.metal): Metal evaluates the
// predicates with LLVM undef semantics, i.e. an undefined operand only makes the result undefined
// when it matters:  A && B with A = true, B unset -> undefined -> absent;  A || B with A unset,
// B = true -> true -> present;  A ? B : C with A unset and B == C -> B;  and
// air.normalize_function_constant_predicate(undefined) -> false.
// This abstract interpreter runs the static initialiser (llvm.global_ctors) over the lattice
// Unknown < Const < Undef, following branches on constant conditions; a branch on an undefined
// condition takes the false edge (measured: fixture fc3) and a switch its first case.  Its result
// decides the implicit predicate globals.  Across the randomized test (tests/fcrand.py) the present
// sets it gives match Metal in every compiled case.  Uses of an unset constant in the function BODY
// are not guessed: resolve_unset_in_body refuses a variant in which one decides anything (ABI v4;
// ABI v3 materialised them as 0, fix round 2 took the false side of selects and branches, and both
// disagreed with Metal, verify3 item 1).  The hardened compiler before ABI v3 evaluated the
// predicates with the unset constants as 0, which made Shadow*BlurFragment's
// is_radius_low_priority true and bound buffer(2) where Metal has none (verifier must-fix 7).
struct AV { enum K { Unknown, C, U } k = Unknown; Constant *c = nullptr; };
static AV avC(Constant *c) { AV v; if (!c || isa<UndefValue>(c)) { v.k = AV::U; return v; } v.k = AV::C; v.c = c; return v; }
static AV avU() { AV v; v.k = AV::U; return v; }
static AV meet(const AV &a, const AV &b) {
  if (a.k == AV::Unknown) return b;
  if (b.k == AV::Unknown) return a;
  if (a.k == AV::U || b.k == AV::U) return avU();
  return a.c == b.c ? a : avU();
}
static bool sameAV(const AV &a, const AV &b) { return a.k == b.k && a.c == b.c; }

static Constant *foldCtorAirCall(CallInst *CI, ArrayRef<Constant *> args);
static std::string ctorCallStr(CallInst *CI, ArrayRef<Constant *> args);
// Fix round 6 (fc6 shadow 3 shader 27, found while fixing verify6 item 3): an LLVM floating-point
// arithmetic instruction of the static initialiser carries Metal's fast-math flags.  When it CREATES an
// inf or a NaN from finite operands (an overflow, x / 0, 0 / 0) under `ninf` / `nnan`, Metal's answer
// is not reproducible: `signbit(float(K) * F - 0)` with K = -128 is false for F = FLT_MAX and true for
// F = 1e38 (both products are -inf; fixture tests/fixtures/v6/fmf_poison.metal).  Such an initialiser is
// not evaluated, and what it decides is refused.  Measured, everything else follows IEEE in Metal and
// is evaluated as IEEE here: an inf / NaN that is already there and propagates (`half(70000) + 0` >=
// 255.5 is true, overflow_body), comparisons with inf / NaN operands (`x >= -INFINITY`, pun_probe's
// `as_type<half2>(F).y == -7.5h`), and the AIR conversions (bool(NaN) true, half(65535) inf).  fc6 runs:
// without this rule 1 presence mismatch in about 10,000 all-set cases (shader 27); refusing on every
// flag violation instead refused a third of all cases that Metal answers the IEEE way.
static bool fpNonFinite(Constant *c, bool inf, bool nan) {
  auto chk = [&](Constant *e) { auto *f = dyn_cast_or_null<ConstantFP>(e); return f && ((inf && f->getValueAPF().isInfinity()) || (nan && f->getValueAPF().isNaN())); };
  if (auto *VT = dyn_cast<FixedVectorType>(c->getType())) {
    for (unsigned i = 0; i < VT->getNumElements(); i++) if (chk(c->getAggregateElement(i))) return true;
    return false;
  }
  return chk(c);
}
static std::string fmfPoison(Instruction &I, ArrayRef<Constant *> ops, Constant *res) {
  auto *BO = dyn_cast<BinaryOperator>(&I);
  if (!BO || !BO->getType()->isFPOrFPVectorTy() || !res) return "";
  bool ninf = BO->hasNoInfs(), nnan = BO->hasNoNaNs();
  if (!ninf && !nnan) return "";
  for (Constant *c : ops) if (!c || fpNonFinite(c, true, true)) return "";          // an inf / NaN operand propagates: IEEE
  if (fpNonFinite(res, ninf, nnan)) return str(&I) + " creates an inf / NaN from finite operands under Metal's fast-math flags (poison; Metal's value is not reproducible)";
  return "";
}
struct CtorEval {
  bool ok = true; std::string why;
  std::map<const GlobalVariable *, AV> mem;
  std::map<CallInst *, AV> normalize;      // value of every normalize call reached
};
static CtorEval abstract_eval_ctors(Module &M, const std::map<int, std::vector<double>> &fc) {
  CtorEval E;
  GlobalVariable *ctors = M.getGlobalVariable("llvm.global_ctors");
  if (!ctors || !ctors->hasInitializer()) return E;
  auto *CA = dyn_cast<ConstantArray>(ctors->getInitializer());
  if (!CA) { E.ok = false; E.why = "global_ctors not an array"; return E; }
  const DataLayout &DL = M.getDataLayout();
  std::map<const GlobalVariable *, std::set<const BasicBlock *>> storeBlocks;
  for (Use &U : CA->operands()) {
    auto *CS = dyn_cast<ConstantStruct>(U.get()); if (!CS) continue;
    auto *Fn = dyn_cast<Function>(CS->getOperand(1)->stripPointerCasts()); if (!Fn || Fn->isDeclaration()) continue;
    for (BasicBlock &BB : *Fn) for (Instruction &I : BB) if (auto *SI = dyn_cast<StoreInst>(&I))
      if (auto *G = dyn_cast<GlobalVariable>(SI->getPointerOperand()->stripPointerCasts())) storeBlocks[G].insert(&BB);
  }
  for (Use &U : CA->operands()) {
    auto *CS = dyn_cast<ConstantStruct>(U.get()); if (!CS) continue;
    auto *Fn = dyn_cast<Function>(CS->getOperand(1)->stripPointerCasts()); if (!Fn || Fn->isDeclaration()) continue;
    std::map<const Value *, AV> val;
    std::set<std::pair<const BasicBlock *, const BasicBlock *>> edges;
    std::set<const BasicBlock *> live{&Fn->getEntryBlock()};
    auto get = [&](Value *v) -> AV {
      if (auto *C = dyn_cast<Constant>(v)) { if (isa<GlobalValue>(C)) return avU(); return avC(C); }
      auto it = val.find(v); return it == val.end() ? AV() : it->second;
    };
    // An instruction with an undefined operand: LLVM's own constant folder decides, with the unset
    // constants as `undef` (Metal's specialiser is LLVM and folds them the same way: e.g.
    // `and undef, 0` -> 0, `or undef, -1` -> -1, `icmp sgt undef, C` -> false,
    // `icmp eq undef, C` -> undef, `select undef, X, Y` -> Y).  A result the folder leaves
    // undef (or cannot fold) stays undefined.
    auto foldU = [&](Instruction *I) -> AV {
      SmallVector<Constant *, 4> ops;
      for (Value *op : I->operands()) {
        AV v = get(op);
        ops.push_back(v.k == AV::C ? v.c : UndefValue::get(op->getType()));
      }
      Constant *res = ConstantFoldInstOperands(I, ops, DL, nullptr);
      if (!res || isa<UndefValue>(res)) return avU();
      return avC(res);
    };
    ReversePostOrderTraversal<Function *> RPOT(Fn);
    for (int iter = 0; iter < 64; iter++) {
      bool changed = false;
      auto setv = [&](Instruction *I, AV v) { AV o = val.count(I) ? val[I] : AV(); AV n = meet(o, v); if (!sameAV(o, n)) { val[I] = n; changed = true; } };
      auto edge = [&](BasicBlock *f, BasicBlock *t) { if (edges.insert({f, t}).second) changed = true; if (live.insert(t).second) changed = true; };
      for (BasicBlock *BB : RPOT) {
        if (!live.count(BB)) continue;
        for (Instruction &I : *BB) {
          if (auto *PN = dyn_cast<PHINode>(&I)) {
            AV r;
            for (unsigned i = 0; i < PN->getNumIncomingValues(); i++)
              if (edges.count({PN->getIncomingBlock(i), BB})) r = meet(r, get(PN->getIncomingValue(i)));
            setv(&I, r); continue;
          }
          if (auto *LI = dyn_cast<LoadInst>(&I)) {
            auto *G = dyn_cast<GlobalVariable>(LI->getPointerOperand()->stripPointerCasts());
            if (!G) { setv(&I, avU()); continue; }
            if (G->getSection() == "air.fc_initializer") {
              int idx = fcIndexOfGlobal(G->getName());
              setv(&I, idx >= 0 && fc.count(idx) ? avC(fcLoadConstant(G, LI->getType(), fc.at(idx), DL)) : avU());
              continue;
            }
            auto m = E.mem.find(G);
            if (m != E.mem.end()) {
              if (m->second.k == AV::C) { Constant *pc = punStored(m->second.c, LI->getType(), DL); setv(&I, pc ? avC(pc) : avU()); }
              else setv(&I, m->second);
              continue;
            }
            if (G->hasInitializer() && G->getInitializer()->getType() == LI->getType()) setv(&I, avC(G->getInitializer()));
            else setv(&I, avU());
            continue;
          }
          if (auto *SI = dyn_cast<StoreInst>(&I)) {
            auto *G = dyn_cast<GlobalVariable>(SI->getPointerOperand()->stripPointerCasts());
            if (!G) continue;
            AV v = get(SI->getValueOperand());
            if (v.k == AV::Unknown) continue;
            AV &slot = E.mem[G];
            AV n = storeBlocks[G].size() > 1 ? meet(slot, v) : v;       // one store site: last write wins
            if (!sameAV(slot, n)) { slot = n; changed = true; }
            continue;
          }
          if (auto *CI = dyn_cast<CallInst>(&I)) {
            Function *cf = CI->getCalledFunction();
            StringRef nm = cf ? cf->getName() : "";
            if (nm.starts_with("air.normalize_function_constant_predicate")) {
              AV x = get(argOperand(CI, 0));
              if (x.k == AV::Unknown) continue;
              AV r;
              if (x.k == AV::U) r = avC(Constant::getNullValue(CI->getType()));
              else { auto *ci = dyn_cast<ConstantInt>(x.c); r = avC(ConstantInt::get(CI->getType(), ci ? !ci->isZero() : 0)); }
              setv(&I, r); E.normalize[CI] = val[&I];
              continue;
            }
            if (nm == "air.is_function_constant_defined") {
              auto *G = dyn_cast<GlobalVariable>(argOperand(CI, 0)->stripPointerCasts());
              int idx = G ? fcIndexOfGlobal(G->getName()) : -1;
              setv(&I, avC(ConstantInt::getBool(CI->getContext(), idx >= 0 && fc.count(idx))));
              continue;
            }
            if (cf && cf->isIntrinsic() && (cf->getIntrinsicID() == Intrinsic::lifetime_start || cf->getIntrinsicID() == Intrinsic::lifetime_end)) continue;
            if (cf && cf->isDeclaration() && nm.starts_with("air.")) {
              // fix round 5 (verify5 item 2 + minor): an AIR conversion with a constant operand folds
              // (foldCtorAirCall); an undefined operand makes it undefined.  What does not fold (NaN /
              // out-of-range float -> integer: poison in Metal; any other AIR function, which Metal
              // rejects in a static initialiser) leaves the initialiser unevaluated: the predicates it
              // computes are then refused, never guessed.
              SmallVector<Constant *, 3> args; bool unk = false, und = false;
              for (Value *op : CI->args()) { AV x = get(op); if (x.k == AV::Unknown) unk = true; else if (x.k == AV::U) und = true; else args.push_back(x.c); }
              if (unk) continue;
              if (und) { setv(&I, avU()); continue; }
              if (Constant *r = foldCtorAirCall(CI, args)) { setv(&I, avC(r)); continue; }
              E.ok = false; E.why = ctorCallStr(CI, args) + " in the static initialiser does not fold"; return E;
            }
            E.ok = false; E.why = "call to " + nm.str() + " in the static initialiser"; return E;
          }
          if (auto *Br = dyn_cast<BranchInst>(&I)) {
            if (Br->isUnconditional()) { edge(BB, Br->getSuccessor(0)); continue; }
            AV c = get(Br->getCondition());
            if (c.k == AV::Unknown) continue;
            // branch on undef: Metal takes the false edge (successor 1), as LLVM SCCP's
            // "resolve undef branch" does (measured: fixture fc3, predicate Q7 = A ? .. : .. with A unset).
            if (c.k == AV::U) { edge(BB, Br->getSuccessor(1)); continue; }
            edge(BB, cast<ConstantInt>(c.c)->isOne() ? Br->getSuccessor(0) : Br->getSuccessor(1));
            continue;
          }
          if (auto *SW = dyn_cast<SwitchInst>(&I)) {
            AV c = get(SW->getCondition());
            if (c.k == AV::Unknown) continue;
            // switch on undef: the first case (LLVM SCCP's "resolve undef switch"; ABI v4, measured
            // in the function body by the randomized differential test, tests/fcrand.py).  v3 took
            // the default edge here.
            if (c.k == AV::U) { edge(BB, SW->getNumCases() ? SW->case_begin()->getCaseSuccessor() : SW->getDefaultDest()); continue; }
            edge(BB, SW->findCaseValue(cast<ConstantInt>(c.c))->getCaseSuccessor());
            continue;
          }
          if (isa<ReturnInst>(&I) || isa<UnreachableInst>(&I)) continue;
          if (auto *Sel = dyn_cast<SelectInst>(&I)) {
            AV c = get(Sel->getCondition()), t = get(Sel->getTrueValue()), f = get(Sel->getFalseValue());
            if (c.k == AV::Unknown) continue;
            if (c.k == AV::C) {
              AV r = cast<ConstantInt>(c.c)->isOne() ? t : f;
              if (r.k != AV::Unknown) setv(&I, r);
              continue;
            }
            if (t.k == AV::Unknown || f.k == AV::Unknown) continue;
            // select undef, X, Y: LLVM's constant folder (ConstantFoldSelectInstruction) gives Y
            // unless X is undef, and so does Metal (fixtures fcsem/fc2/fc3, 405 cases, regression
            // test unset_fc_semantics).  v2 of this file returned "undefined" unless X == Y, which
            // disagreed with Metal for A ? B : C and (A || B) && C with A unset (verify2 item 5).
            setv(&I, foldU(&I));
            continue;
          }
          // generic: constant-fold; an undefined operand makes the result undefined unless absorbed
          SmallVector<Constant *, 4> ops; bool anyU = false, anyUnknown = false;
          for (Value *op : I.operands()) {
            AV v = get(op);
            if (v.k == AV::Unknown) anyUnknown = true; else if (v.k == AV::U) anyU = true; else ops.push_back(v.c);
          }
          if (anyUnknown) continue;
          if (anyU) { setv(&I, foldU(&I)); continue; }
          Constant *res = ConstantFoldInstOperands(&I, ops, DL, nullptr);
          if (!res) { E.ok = false; E.why = "cannot fold " + str(&I); return E; }
          if (std::string w = fmfPoison(I, ops, res); !w.empty()) { E.ok = false; E.why = w + " in the static initialiser"; return E; }
          setv(&I, avC(res));
        }
      }
      if (!changed) break;
      if (iter == 63) { E.ok = false; E.why = "static initialiser did not converge"; return E; }
    }
  }
  return E;
}
