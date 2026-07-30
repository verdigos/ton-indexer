#pragma once

// AbiKernel: the load-bearing resolution layer (W2). A faithful C++ port of
// tolk-abi-to-typescript's src/types-kernel.ts -- SymTable, aliasTargetOf,
// structFieldsOf (monomorph substitution + cell-side client_ty_idx swap +
// uLabelTyIdx back-ref), createLabelsForUnion (with isStructWithItsOwnLabel /
// renderTy dedup rules), plus the LOAD-TIME RESOLUTION PASS the plan asks for
// (docs/ton-abi-plan.md T2.1/T2.2): flatten interned indices into resolved
// field vectors and build sorted union prefix-dispatch tables.
//
// NOT ported: calcWidthOnStack and everything isForStack -- stack/get-method
// domain is out of scope (plan decision 1). structFieldsOf here is CELL side
// only (isForStack == false, always).
//
// THREE NAMED INVARIANTS (plan final-review F3) -- realized as follows:
//  (i)   Union label tables are a USE-SITE property: the SAME interned union
//        arrives with a DIFFERENT uLabel from different generic instantiations
//        (e.g. GenericPair<int32,int64>.value labels as T1|T2, not int32|int64).
//        So labels are computed per (union ty_idx, uLabelTyIdx) pair at
//        resolve_union()/create_labels_for_union() call time and NEVER cached
//        under the union ty_idx alone. Only the prefix DISPATCH ORDER (which is
//        use-site independent -- prefixes don't vary with uLabel) is cached per
//        union ty_idx.
//  (ii)  Custom-hooked alias/enum/struct nodes SURVIVE resolution as nodes: the
//        kernel NEVER checks custom_pack_unpack and NEVER collapses a custom
//        node into its target on its own. Consumers (the W8 walker) MUST check
//        custom_pack_unpack BEFORE calling alias_target_of/struct_fields_of --
//        exactly the order in dynamic-serialization.ts:335-374. Eager precompute
//        of a custom node's target is harmless dead data (the walker won't ask).
//  (iii) Prefix-freedom is validated by the LOADER (AbiLoader union prefix-
//        freedom check) BEFORE we sort, which makes the sorted dispatch order
//        provably equivalent to the reference's declaration-order first-match
//        (under prefix-freedom at most one variant prefix can match a slice, so
//        order is irrelevant). See the comment on build_union_dispatch().

#include "AbiModel.h"

#include "td/utils/Status.h"

#include <cstdint>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace ton_abi {

// A struct field after resolution. `orig` points into the ContractABI (kept
// alive by the non-owning ref the kernel holds) and carries name/default_value/
// description/original-client_ty_idx; `ty_idx` is the RESOLVED type (monomorph
// then cell-side client swap); `u_label_ty_idx` is the union-label back-ref.
// Mirrors the objects structFieldsOf returns (types-kernel.ts:69-95).
struct ResolvedField {
  const ABIStructField *orig = nullptr;
  int ty_idx = 0;
  std::optional<int> u_label_ty_idx;

  const std::string &name() const { return orig->name; }
};

// aliasTargetOf result (types-kernel.ts:103-120).
struct AliasTarget {
  int ty_idx = 0;
  std::optional<int> u_label_ty_idx;
};

// One union variant plus its computed TS-style `$` label. `variant_ty_idx` and
// the prefix fields are the ACTUAL (instantiated) variant; `label_str` /
// `has_value_field` come from createLabelsForUnion (types-kernel.ts:235-277)
// and may derive from the GENERIC variant when uLabelTyIdx is supplied.
struct LabeledVariant {
  int variant_ty_idx = 0;
  std::uint32_t prefix_num = 0;
  int prefix_len = 0;
  bool is_prefix_implicit = false;
  std::string label_str;
  bool has_value_field = false;
};

// A resolved union at a specific use-site. `variants` are labeled and kept in
// DECLARATION order (index-stable for the emitter's std::variant and for label
// correctness). `dispatch_order` holds indices into `variants`, sorted ascending
// by (prefix_len, prefix_num), EXCLUDING a trailing `void` variant. `has_void`
// mirrors the reference's "trailing void variant" special case
// (dynamic-serialization.ts:378-411) -- when true, variants.back() is that void.
struct ResolvedUnion {
  std::vector<LabeledVariant> variants;
  std::vector<std::size_t> dispatch_order;
  bool has_void = false;
};

class AbiKernel {
 public:
  // Non-owning: `abi` MUST outlive the kernel. The loader already validated
  // OOB/arity/cycles/prefix-freedom; create() additionally re-verifies every ty
  // index is in bounds (cheap) so the internal render/label recursion may trust
  // ty_ref() without per-access bounds checks, and it eagerly builds the
  // use-site-independent caches (struct fields, alias targets, union dispatch
  // orders). Fail-closed: any resolution error aborts creation with a Status.
  static td::Result<AbiKernel> create(const ContractABI &abi);

  AbiKernel(AbiKernel &&) = default;
  AbiKernel &operator=(AbiKernel &&) = default;
  AbiKernel(const AbiKernel &) = delete;
  AbiKernel &operator=(const AbiKernel &) = delete;

  // ---- SymTable lookups (types-kernel.ts:35-61) ----
  td::Result<const ABIStruct *> get_struct(const std::string &name) const;
  td::Result<const ABIAlias *> get_alias(const std::string &name) const;
  td::Result<const ABIEnum *> get_enum(const std::string &name) const;
  td::Result<const Ty *> ty_by_idx(int ty_idx) const;

  // ---- resolution ----
  // structFieldsOf, CELL side. Returns a pointer to the cached resolved-field
  // vector. Errors if ty_idx is not a StructRef.
  td::Result<const std::vector<ResolvedField> *> struct_fields_of(int ty_idx) const;

  // aliasTargetOf. Errors if ty_idx is not an AliasRef.
  td::Result<AliasTarget> alias_target_of(int ty_idx) const;

  // createLabelsForUnion. Computed per call (labels are a USE-SITE property --
  // invariant (i)); `u_label_ty_idx` points to the original (generic) union
  // whose variant labels to borrow, when set.
  td::Result<std::vector<LabeledVariant>> create_labels_for_union(
      const std::vector<UnionVariant> &variants, std::optional<int> u_label_ty_idx) const;

  // Sorted + labeled union table for a (union ty_idx, uLabel) use-site.
  td::Result<ResolvedUnion> resolve_union(int union_ty_idx, std::optional<int> u_label_ty_idx) const;

 private:
  explicit AbiKernel(const ContractABI &abi) : abi_(&abi) {}

  // Trusted in-bounds (create() guarantees it). Used by the deep render/label
  // recursion where threading a Result through every switch arm would be noise.
  const Ty &ty_ref(int idx) const { return abi_->unique_types[static_cast<std::size_t>(idx)]; }

  // renderTy / createLabel / isStructWithItsOwnLabel (types-kernel.ts:129-230).
  std::string render_ty(int ty_idx) const;
  std::string render_type_args(const std::vector<int> &args) const;
  std::string create_label(int ty_idx) const;
  bool is_struct_with_own_label(int ty_idx) const;

  // Eager-pass builders (create() only).
  td::Status build_index();
  td::Status build_resolution();
  td::Result<std::vector<ResolvedField>> compute_struct_fields(int ty_idx) const;
  td::Result<AliasTarget> compute_alias_target(int ty_idx) const;
  void build_union_dispatch(int union_ty_idx);

  const ContractABI *abi_;

  std::unordered_map<std::string, const ABIStruct *> structs_;
  std::unordered_map<std::string, const ABIAlias *> aliases_;
  std::unordered_map<std::string, const ABIEnum *> enums_;
  std::unordered_map<int, const ABIStructInstantiation *> struct_inst_;
  std::unordered_map<int, const ABIAliasInstantiation *> alias_inst_;

  // Use-site-independent caches, populated in create(); read-only afterwards
  // (so concurrent reads are lock-free -- the bridge holds kernels static and
  // immortal, plan decision 16).
  std::unordered_map<int, std::vector<ResolvedField>> fields_cache_;
  std::unordered_map<int, AliasTarget> alias_cache_;
  std::unordered_map<int, std::vector<std::size_t>> union_dispatch_cache_;
  std::unordered_map<int, bool> union_has_void_cache_;
};

}  // namespace ton_abi
