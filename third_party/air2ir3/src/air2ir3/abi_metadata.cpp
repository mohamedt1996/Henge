// SPDX-License-Identifier: MIT
// ABI constants, shared helpers (die, argOperand, JSON escaping) and the AIR argument/output metadata parser.
// Part of src/air2ir3.cpp's translation unit, #included from it in order; not compiled on its own.

// ---------------------------------------------------------------- ABI constants (BINDING-ABI.md) --
static const int ABI_VERSION = 6;
static const int CACHE_FORMAT_VERSION = 5;      // format 5 (ABI 6): function-constant values exact in their declared type (verify6 must-fix 2)
static const int ABI_MAX_TEXTURES = 16;          // Metal texture(n), n < 16  -> T# n (non-bindless)
static const int ABI_MAX_SAMPLERS = 16;          // Metal sampler(n) + constexpr samplers share S# 0..15 (constexpr: lowest free, ABI v4)
// ABI v6: the non-bindless UBO table a8xx honours has 16 entries (freedreno advertises
// max_const_buffers = 16 for ir3; Turnip binds app UBOs bindless; q1n1 A16 render run 1: a load from
// UBO 31 read zeros although its descriptor was loaded).  Every UBO index a variant uses is < 16.
// The driver table (device-buffer VAs) sits at R.driver_table_ubo = 1 + the highest constant buffer
// the function declares (0 when it declares none), and ir3's own driver UBOs follow it.
static const int ABI_UBO_SLOTS = 16;
static const int ABI_TG_TABLE_OFFSET = 256;      //   ... + threadgroup(n) byte offsets at 256 + 4n (BINDING-ABI.md 3.8)
static const int ABI_MAX_UBO_INDEX = ABI_UBO_SLOTS - 1;   // app UBOs, the driver table, ir3 driver UBOs: all <= 15 (ABI v6)
// the UBO table size a compile checks against: 16, or the probe-only --test-ubo-slots N (q1n1 render probe_ubo_hi)
static int uboSlots(const AirOptions &o) { return o.testUboSlots > 0 ? std::min(o.testUboSlots, 32) : ABI_UBO_SLOTS; }
static const int ABI_MAX_RTS = 8;                // A6XX_MAX_RENDER_TARGETS
static const int ABI_FS_BINDLESS_SET = 4;        // ir3_shader_descriptor_set(MESA_SHADER_FRAGMENT)
static const int ABI_FB_READ_SLOT = 32 + 32 - 1 - ABI_MAX_RTS;  // IR3_BINDLESS_IMAGE_OFFSET + IR3_BINDLESS_IMAGE_COUNT - 1 - max_rts

[[noreturn]] static void die(const std::string &m) { throw AirError(m); }
// The only way this compiler reads a call operand. An index past the call's operand count is a refusal: LLVM's
// getArgOperand() asserts on it, and that assertion aborts the Metal client. WindowServer died this way in
// installed-fresh-20261020-v13 and -v14 on CoreAnimation's compute_apl, whose AIR 2.2 texture read has four operands
// where the AIR 2.9 one has six (docs/WINDOWSERVER-APL-ABORT-20261020.md).
static Value *argOperand(const CallInst *CI, unsigned i) {
  if (i >= CI->arg_size()) {
    const Function *F = CI->getCalledFunction();
    die("call to " + (F ? F->getName().str() : std::string("an indirect callee")) + " has " + std::to_string(CI->arg_size()) +
        " operands, operand " + std::to_string(i) + " was expected (an operand layout this compiler does not know)");
  }
  return CI->getArgOperand(i);
}
static bool containsPointer(Type *T) {
  if (T->isPointerTy()) return true;
  if (auto *S = dyn_cast<StructType>(T)) { for (Type *E : S->elements()) if (containsPointer(E)) return true; return false; }
  if (auto *A = dyn_cast<ArrayType>(T)) return containsPointer(A->getElementType());
  if (auto *V = dyn_cast<VectorType>(T)) return containsPointer(V->getElementType());
  return false;
}
static std::string str(const Value *v) {
  std::string s; raw_string_ostream os(s); v->print(os); return s;
}
static std::string jesc(const std::string &s) {
  std::string o;
  for (char c : s) {
    if (c == '"' || c == '\\') { o += '\\'; o += c; }
    else if ((unsigned char)c < 0x20) { char b[8]; snprintf(b, sizeof b, "\\u%04x", c); o += b; }
    else o += c;
  }
  return o;
}

// ---------------------------------------------------------------- metadata --
// Argument / output metadata, parsed BEFORE specialisation (GlobalOpt may delete the implicit
// globals that the metadata points at).
struct ArgInfo {
  std::string kind;          // air.vertex_input, air.fragment_input, air.position, air.render_target, ...
  std::string name;          // user(...) / generated(...) varying name
  std::string argName, typeName;
  int argno = -1;            // input argument number (-1 for outputs)
  int location = -1;         // air.location_index / render target index
  int dualIndex = 0;         // air.render_target's second positional: [[color(n), index(1)]] = 1
  std::string locGlobal;     // location given by a function-constant expression global
  int addrspace = -1;        // for buffers
  int64_t bufferSize = -1;   // explicit AIR argument buffer extent, if present
  std::string interp = "perspective", sampling = "center";
  bool gated = false;        // [[function_constant(...)]]
  std::string predGlobal;    // predicate global (e.g. __metal_implicit_fc_pred_3 or an FC initialiser)
  bool active = true;        // gets a binding: present, or read by the specialised code
  bool present = true;       // Metal's notion: predicate true (or not gated)
  bool used = true;          // (inputs) argument has uses after specialisation
  std::string access;        // air.read / air.sample / air.write / air.read_write
};
static std::string mdstr(const Metadata *m) {
  if (auto *s = dyn_cast_or_null<MDString>(m)) return s->getString().str();
  return "";
}
static int mdint(const Metadata *m) {
  if (auto *c = dyn_cast_or_null<ConstantAsMetadata>(m))
    if (auto *ci = dyn_cast<ConstantInt>(c->getValue())) return (int)ci->getSExtValue();
  return -1;
}
static std::string mdglobal(const Metadata *m) {
  if (auto *v = dyn_cast_or_null<ValueAsMetadata>(m))
    if (auto *g = dyn_cast<GlobalValue>(v->getValue()->stripPointerCasts())) return g->getName().str();
  return "";
}
// Layout (26A428 AIR 2.9):
//   output: [kind, positional..., "key", value, ...]            gated: ["air.function_constant", !pred, kind, ...]
//   input:  [argno, kind, positional..., "key", value, ...]     gated: [argno, "air.function_constant", !pred, kind, ...]
//   air.render_target positional: <index> [<dual-source index>]; air.location_index value may be
//   an i32 or a pointer to a function-constant expression global (__metal_implicit_attr_int_expr_N).
// Prototype bug fixed here: for gated render targets the index was read from the predicate slot
// (giving colour -1 = FRAG_RESULT_SAMPLE_MASK).
static ArgInfo parseArg(const MDNode *n, bool isOutput) {
  ArgInfo a;
  unsigned i = isOutput ? 0 : 1;
  if (!isOutput) a.argno = mdint(n->getOperand(0));
  unsigned k = i;
  if (k < n->getNumOperands()) a.kind = mdstr(n->getOperand(k));
  if (a.kind == "air.function_constant") {
    a.gated = true;
    if (auto *pn = dyn_cast_or_null<MDNode>(n->getOperand(k + 1).get()))
      if (pn->getNumOperands() > 0) a.predGlobal = mdglobal(pn->getOperand(0));
    k += 2;
    a.kind = k < n->getNumOperands() ? mdstr(n->getOperand(k)) : "";
  }
  if (a.kind == "air.render_target" && k + 1 < n->getNumOperands()) a.location = mdint(n->getOperand(k + 1));
  if (a.kind == "air.render_target" && k + 2 < n->getNumOperands()) a.dualIndex = std::max(0, mdint(n->getOperand(k + 2)));
  for (unsigned j = k + 1; j < n->getNumOperands(); j++) {
    std::string s = mdstr(n->getOperand(j));
    if (s == "air.location_index" && j + 1 < n->getNumOperands()) {
      int v = mdint(n->getOperand(j + 1));
      if (v >= 0) a.location = v; else a.locGlobal = mdglobal(n->getOperand(j + 1));
    }
    else if (s == "air.address_space" && j + 1 < n->getNumOperands()) a.addrspace = mdint(n->getOperand(j + 1));
    else if (s == "air.buffer_size" && j + 1 < n->getNumOperands()) {
      int size = mdint(n->getOperand(j + 1));
      if (size < 0) die("invalid air.buffer_size metadata");
      a.bufferSize = size;
    }
    else if (s == "air.perspective" || s == "air.no_perspective" || s == "air.flat") a.interp = s.substr(4);
    else if (s == "air.center" || s == "air.centroid" || s == "air.sample") {
      if (a.kind == "air.texture" && s == "air.sample") a.access = "sample"; else a.sampling = s.substr(4);
    }
    else if (s == "air.read" || s == "air.write" || s == "air.read_write") a.access = s.substr(4);
    else if (s == "air.arg_name" && j + 1 < n->getNumOperands()) a.argName = mdstr(n->getOperand(j + 1));
    else if (s == "air.arg_type_name" && j + 1 < n->getNumOperands()) a.typeName = mdstr(n->getOperand(j + 1));
    else if (s.rfind("user(", 0) == 0 || s.rfind("generated(", 0) == 0) a.name = s;
  }
  return a;
}

// Function constants declared by the module (!air.function_constants).
struct FcDecl { int index; std::string type, name, global; };
static std::vector<FcDecl> parseFcDecls(Module &M) {
  std::vector<FcDecl> r;
  if (NamedMDNode *N = M.getNamedMetadata("air.function_constants"))
    for (MDNode *E : N->operands()) {
      if (E->getNumOperands() < 4) continue;
      FcDecl d; d.global = mdglobal(E->getOperand(0)); d.type = mdstr(E->getOperand(1));
      d.name = mdstr(E->getOperand(2)); d.index = mdint(E->getOperand(3));
      r.push_back(d);
    }
  std::sort(r.begin(), r.end(), [](const FcDecl &a, const FcDecl &b) { return a.index < b.index; });
  return r;
}
static int fcIndexOfGlobal(StringRef nm) {
  size_t p = nm.find(".MTL_FC_INIT_");
  if (p == StringRef::npos) return -1;
  return std::atoi(nm.substr(p + 13).str().c_str());
}
