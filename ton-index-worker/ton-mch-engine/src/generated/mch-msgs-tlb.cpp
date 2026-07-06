#include "mch-msgs-tlb.h"
/*
 *
 *  AUTO-GENERATED FROM `S:/Dev/ton-indexer-clear/ton-index-worker/ton-mch-engine/tlb/mch-msgs.tlb`
 *
 */
// uses built-in type `##`
// uses built-in type `#<`
// uses built-in type `#<=`
// uses built-in type `Cell`
// uses built-in type `uint`
// uses built-in type `bits`
// uses built-in type `int8`
// uses built-in type `int32`
// uses built-in type `uint32`
// uses built-in type `uint64`
// uses built-in type `bits256`

namespace mchmsg {

namespace gen {
using namespace ::tlb;
using td::Ref;
using vm::CellSlice;
using vm::Cell;
using td::RefInt256;

//
// code for type `Maybe`
//

int Maybe::check_tag(const vm::CellSlice& cs) const {
  switch (get_tag(cs)) {
  case nothing:
    return cs.have(1) ? nothing : -1;
  case just:
    return cs.have(1) ? just : -1;
  }
  return -1;
}

bool Maybe::skip(vm::CellSlice& cs) const {
  switch (get_tag(cs)) {
  case nothing:
    return cs.advance(1);
  case just:
    return cs.advance(1)
        && X_.skip(cs);
  }
  return false;
}

bool Maybe::validate_skip(int* ops, vm::CellSlice& cs, bool weak) const {
  switch (get_tag(cs)) {
  case nothing:
    return cs.advance(1);
  case just:
    return cs.advance(1)
        && X_.validate_skip(ops, cs, weak);
  }
  return false;
}

bool Maybe::unpack(vm::CellSlice& cs, Maybe::Record_nothing& data) const {
  return cs.fetch_ulong(1) == 0;
}

bool Maybe::unpack_nothing(vm::CellSlice& cs) const {
  return cs.fetch_ulong(1) == 0;
}

bool Maybe::cell_unpack(Ref<vm::Cell> cell_ref, Maybe::Record_nothing& data) const {
  if (cell_ref.is_null()) { return false; }
  auto cs = load_cell_slice(std::move(cell_ref));
  return unpack(cs, data) && cs.empty_ext();
}

bool Maybe::cell_unpack_nothing(Ref<vm::Cell> cell_ref) const {
  if (cell_ref.is_null()) { return false; }
  auto cs = load_cell_slice(std::move(cell_ref));
  return unpack_nothing(cs) && cs.empty_ext();
}

bool Maybe::unpack(vm::CellSlice& cs, Maybe::Record_just& data) const {
  return cs.fetch_ulong(1) == 1
      && X_.fetch_to(cs, data.value);
}

bool Maybe::unpack_just(vm::CellSlice& cs, Ref<CellSlice>& value) const {
  return cs.fetch_ulong(1) == 1
      && X_.fetch_to(cs, value);
}

bool Maybe::cell_unpack(Ref<vm::Cell> cell_ref, Maybe::Record_just& data) const {
  if (cell_ref.is_null()) { return false; }
  auto cs = load_cell_slice(std::move(cell_ref));
  return unpack(cs, data) && cs.empty_ext();
}

bool Maybe::cell_unpack_just(Ref<vm::Cell> cell_ref, Ref<CellSlice>& value) const {
  if (cell_ref.is_null()) { return false; }
  auto cs = load_cell_slice(std::move(cell_ref));
  return unpack_just(cs, value) && cs.empty_ext();
}

bool Maybe::pack(vm::CellBuilder& cb, const Maybe::Record_nothing& data) const {
  return cb.store_long_bool(0, 1);
}

bool Maybe::pack_nothing(vm::CellBuilder& cb) const {
  return cb.store_long_bool(0, 1);
}

bool Maybe::cell_pack(Ref<vm::Cell>& cell_ref, const Maybe::Record_nothing& data) const {
  vm::CellBuilder cb;
  return pack(cb, data) && std::move(cb).finalize_to(cell_ref);
}

bool Maybe::cell_pack_nothing(Ref<vm::Cell>& cell_ref) const {
  vm::CellBuilder cb;
  return pack_nothing(cb) && std::move(cb).finalize_to(cell_ref);
}

bool Maybe::pack(vm::CellBuilder& cb, const Maybe::Record_just& data) const {
  return cb.store_long_bool(1, 1)
      && X_.store_from(cb, data.value);
}

bool Maybe::pack_just(vm::CellBuilder& cb, Ref<CellSlice> value) const {
  return cb.store_long_bool(1, 1)
      && X_.store_from(cb, value);
}

bool Maybe::cell_pack(Ref<vm::Cell>& cell_ref, const Maybe::Record_just& data) const {
  vm::CellBuilder cb;
  return pack(cb, data) && std::move(cb).finalize_to(cell_ref);
}

bool Maybe::cell_pack_just(Ref<vm::Cell>& cell_ref, Ref<CellSlice> value) const {
  vm::CellBuilder cb;
  return pack_just(cb, std::move(value)) && std::move(cb).finalize_to(cell_ref);
}

bool Maybe::print_skip(PrettyPrinter& pp, vm::CellSlice& cs) const {
  switch (get_tag(cs)) {
  case nothing:
    return cs.advance(1)
        && pp.cons("nothing");
  case just:
    return cs.advance(1)
        && pp.open("just")
        && pp.field("value")
        && X_.print_skip(pp, cs)
        && pp.close();
  }
  return pp.fail("unknown constructor for Maybe");
}

bool Maybe::print_skip(JsonPrinter& pp, vm::CellSlice& cs) const {
  switch (get_tag(cs)) {
  case nothing:
    return cs.advance(1)
        && pp.cons("nothing");
  case just:
    return cs.advance(1)
        && pp.open("just")
        && pp.field("value")
        && X_.print_skip(pp, cs)
        && pp.close();
  }
  return pp.fail("unknown constructor for Maybe");
}


//
// code for type `VarUInteger`
//

int VarUInteger::check_tag(const vm::CellSlice& cs) const {
  return var_uint;
}

bool VarUInteger::skip(vm::CellSlice& cs) const {
  int len;
  return cs.fetch_uint_less(m_, len)
      && cs.advance(8 * len);
}

bool VarUInteger::validate_skip(int* ops, vm::CellSlice& cs, bool weak) const {
  int len;
  return cs.fetch_uint_less(m_, len)
      && cs.advance(8 * len);
}

bool VarUInteger::unpack(vm::CellSlice& cs, VarUInteger::Record& data) const {
  return (data.n = m_) >= 0
      && cs.fetch_uint_less(m_, data.len)
      && cs.fetch_uint256_to(8 * data.len, data.value);
}

bool VarUInteger::unpack_var_uint(vm::CellSlice& cs, int& n, int& len, RefInt256& value) const {
  return (n = m_) >= 0
      && cs.fetch_uint_less(m_, len)
      && cs.fetch_uint256_to(8 * len, value);
}

bool VarUInteger::cell_unpack(Ref<vm::Cell> cell_ref, VarUInteger::Record& data) const {
  if (cell_ref.is_null()) { return false; }
  auto cs = load_cell_slice(std::move(cell_ref));
  return unpack(cs, data) && cs.empty_ext();
}

bool VarUInteger::cell_unpack_var_uint(Ref<vm::Cell> cell_ref, int& n, int& len, RefInt256& value) const {
  if (cell_ref.is_null()) { return false; }
  auto cs = load_cell_slice(std::move(cell_ref));
  return unpack_var_uint(cs, n, len, value) && cs.empty_ext();
}

bool VarUInteger::pack(vm::CellBuilder& cb, const VarUInteger::Record& data) const {
  return cb.store_uint_less(m_, data.len)
      && cb.store_int256_bool(data.value, 8 * data.len, false);
}

bool VarUInteger::pack_var_uint(vm::CellBuilder& cb, int len, RefInt256 value) const {
  return cb.store_uint_less(m_, len)
      && cb.store_int256_bool(value, 8 * len, false);
}

bool VarUInteger::cell_pack(Ref<vm::Cell>& cell_ref, const VarUInteger::Record& data) const {
  vm::CellBuilder cb;
  return pack(cb, data) && std::move(cb).finalize_to(cell_ref);
}

bool VarUInteger::cell_pack_var_uint(Ref<vm::Cell>& cell_ref, int len, RefInt256 value) const {
  vm::CellBuilder cb;
  return pack_var_uint(cb, len, std::move(value)) && std::move(cb).finalize_to(cell_ref);
}

bool VarUInteger::print_skip(PrettyPrinter& pp, vm::CellSlice& cs) const {
  int len;
  return pp.open("var_uint")
      && cs.fetch_uint_less(m_, len)
      && pp.field_int(len, "len")
      && pp.fetch_uint256_field(cs, 8 * len, "value")
      && pp.close();
}

bool VarUInteger::print_skip(JsonPrinter& pp, vm::CellSlice& cs) const {
  int len;
  return pp.open("var_uint")
      && cs.fetch_uint_less(m_, len)
      && pp.field_int(len, "len")
      && pp.fetch_uint256_field(cs, 8 * len, "value")
      && pp.close();
}


//
// code for type `MsgAddressExt`
//

int MsgAddressExt::check_tag(const vm::CellSlice& cs) const {
  switch (get_tag(cs)) {
  case addr_none:
    return cs.have(2) ? addr_none : -1;
  case addr_extern:
    return cs.prefetch_ulong(2) == 1 ? addr_extern : -1;
  }
  return -1;
}

bool MsgAddressExt::skip(vm::CellSlice& cs) const {
  switch (get_tag(cs)) {
  case addr_none:
    return cs.advance(2);
  case addr_extern: {
    int len;
    return cs.advance(2)
        && cs.fetch_uint_to(9, len)
        && cs.advance(len);
    }
  }
  return false;
}

bool MsgAddressExt::validate_skip(int* ops, vm::CellSlice& cs, bool weak) const {
  switch (get_tag(cs)) {
  case addr_none:
    return cs.advance(2);
  case addr_extern: {
    int len;
    return cs.fetch_ulong(2) == 1
        && cs.fetch_uint_to(9, len)
        && cs.advance(len);
    }
  }
  return false;
}

bool MsgAddressExt::unpack(vm::CellSlice& cs, MsgAddressExt::Record_addr_none& data) const {
  return cs.fetch_ulong(2) == 0;
}

bool MsgAddressExt::unpack_addr_none(vm::CellSlice& cs) const {
  return cs.fetch_ulong(2) == 0;
}

bool MsgAddressExt::cell_unpack(Ref<vm::Cell> cell_ref, MsgAddressExt::Record_addr_none& data) const {
  if (cell_ref.is_null()) { return false; }
  auto cs = load_cell_slice(std::move(cell_ref));
  return unpack(cs, data) && cs.empty_ext();
}

bool MsgAddressExt::cell_unpack_addr_none(Ref<vm::Cell> cell_ref) const {
  if (cell_ref.is_null()) { return false; }
  auto cs = load_cell_slice(std::move(cell_ref));
  return unpack_addr_none(cs) && cs.empty_ext();
}

bool MsgAddressExt::unpack(vm::CellSlice& cs, MsgAddressExt::Record_addr_extern& data) const {
  return cs.fetch_ulong(2) == 1
      && cs.fetch_uint_to(9, data.len)
      && cs.fetch_bitstring_to(data.len, data.external_address);
}

bool MsgAddressExt::unpack_addr_extern(vm::CellSlice& cs, int& len, Ref<td::BitString>& external_address) const {
  return cs.fetch_ulong(2) == 1
      && cs.fetch_uint_to(9, len)
      && cs.fetch_bitstring_to(len, external_address);
}

bool MsgAddressExt::cell_unpack(Ref<vm::Cell> cell_ref, MsgAddressExt::Record_addr_extern& data) const {
  if (cell_ref.is_null()) { return false; }
  auto cs = load_cell_slice(std::move(cell_ref));
  return unpack(cs, data) && cs.empty_ext();
}

bool MsgAddressExt::cell_unpack_addr_extern(Ref<vm::Cell> cell_ref, int& len, Ref<td::BitString>& external_address) const {
  if (cell_ref.is_null()) { return false; }
  auto cs = load_cell_slice(std::move(cell_ref));
  return unpack_addr_extern(cs, len, external_address) && cs.empty_ext();
}

bool MsgAddressExt::pack(vm::CellBuilder& cb, const MsgAddressExt::Record_addr_none& data) const {
  return cb.store_long_bool(0, 2);
}

bool MsgAddressExt::pack_addr_none(vm::CellBuilder& cb) const {
  return cb.store_long_bool(0, 2);
}

bool MsgAddressExt::cell_pack(Ref<vm::Cell>& cell_ref, const MsgAddressExt::Record_addr_none& data) const {
  vm::CellBuilder cb;
  return pack(cb, data) && std::move(cb).finalize_to(cell_ref);
}

bool MsgAddressExt::cell_pack_addr_none(Ref<vm::Cell>& cell_ref) const {
  vm::CellBuilder cb;
  return pack_addr_none(cb) && std::move(cb).finalize_to(cell_ref);
}

bool MsgAddressExt::pack(vm::CellBuilder& cb, const MsgAddressExt::Record_addr_extern& data) const {
  return cb.store_long_bool(1, 2)
      && cb.store_ulong_rchk_bool(data.len, 9)
      && cb.append_bitstring_chk(data.external_address, data.len);
}

bool MsgAddressExt::pack_addr_extern(vm::CellBuilder& cb, int len, Ref<td::BitString> external_address) const {
  return cb.store_long_bool(1, 2)
      && cb.store_ulong_rchk_bool(len, 9)
      && cb.append_bitstring_chk(external_address, len);
}

bool MsgAddressExt::cell_pack(Ref<vm::Cell>& cell_ref, const MsgAddressExt::Record_addr_extern& data) const {
  vm::CellBuilder cb;
  return pack(cb, data) && std::move(cb).finalize_to(cell_ref);
}

bool MsgAddressExt::cell_pack_addr_extern(Ref<vm::Cell>& cell_ref, int len, Ref<td::BitString> external_address) const {
  vm::CellBuilder cb;
  return pack_addr_extern(cb, len, std::move(external_address)) && std::move(cb).finalize_to(cell_ref);
}

bool MsgAddressExt::print_skip(PrettyPrinter& pp, vm::CellSlice& cs) const {
  switch (get_tag(cs)) {
  case addr_none:
    return cs.advance(2)
        && pp.cons("addr_none");
  case addr_extern: {
    int len;
    return cs.fetch_ulong(2) == 1
        && pp.open("addr_extern")
        && cs.fetch_uint_to(9, len)
        && pp.field_int(len, "len")
        && pp.fetch_bits_field(cs, len, "external_address")
        && pp.close();
    }
  }
  return pp.fail("unknown constructor for MsgAddressExt");
}

bool MsgAddressExt::print_skip(JsonPrinter& pp, vm::CellSlice& cs) const {
  switch (get_tag(cs)) {
  case addr_none:
    return cs.advance(2)
        && pp.cons("addr_none");
  case addr_extern: {
    int len;
    return cs.fetch_ulong(2) == 1
        && pp.open("addr_extern")
        && cs.fetch_uint_to(9, len)
        && pp.field_int(len, "len")
        && pp.fetch_bits_field(cs, len, "external_address")
        && pp.close();
    }
  }
  return pp.fail("unknown constructor for MsgAddressExt");
}

const MsgAddressExt t_MsgAddressExt;

//
// code for type `Anycast`
//

int Anycast::check_tag(const vm::CellSlice& cs) const {
  return anycast_info;
}

bool Anycast::skip(vm::CellSlice& cs) const {
  int depth;
  return cs.fetch_uint_leq(30, depth)
      && 1 <= depth
      && cs.advance(depth);
}

bool Anycast::validate_skip(int* ops, vm::CellSlice& cs, bool weak) const {
  int depth;
  return cs.fetch_uint_leq(30, depth)
      && 1 <= depth
      && cs.advance(depth);
}

bool Anycast::unpack(vm::CellSlice& cs, Anycast::Record& data) const {
  return cs.fetch_uint_leq(30, data.depth)
      && 1 <= data.depth
      && cs.fetch_bitstring_to(data.depth, data.rewrite_pfx);
}

bool Anycast::unpack_anycast_info(vm::CellSlice& cs, int& depth, Ref<td::BitString>& rewrite_pfx) const {
  return cs.fetch_uint_leq(30, depth)
      && 1 <= depth
      && cs.fetch_bitstring_to(depth, rewrite_pfx);
}

bool Anycast::cell_unpack(Ref<vm::Cell> cell_ref, Anycast::Record& data) const {
  if (cell_ref.is_null()) { return false; }
  auto cs = load_cell_slice(std::move(cell_ref));
  return unpack(cs, data) && cs.empty_ext();
}

bool Anycast::cell_unpack_anycast_info(Ref<vm::Cell> cell_ref, int& depth, Ref<td::BitString>& rewrite_pfx) const {
  if (cell_ref.is_null()) { return false; }
  auto cs = load_cell_slice(std::move(cell_ref));
  return unpack_anycast_info(cs, depth, rewrite_pfx) && cs.empty_ext();
}

bool Anycast::pack(vm::CellBuilder& cb, const Anycast::Record& data) const {
  return cb.store_uint_leq(30, data.depth)
      && 1 <= data.depth
      && cb.append_bitstring_chk(data.rewrite_pfx, data.depth);
}

bool Anycast::pack_anycast_info(vm::CellBuilder& cb, int depth, Ref<td::BitString> rewrite_pfx) const {
  return cb.store_uint_leq(30, depth)
      && 1 <= depth
      && cb.append_bitstring_chk(rewrite_pfx, depth);
}

bool Anycast::cell_pack(Ref<vm::Cell>& cell_ref, const Anycast::Record& data) const {
  vm::CellBuilder cb;
  return pack(cb, data) && std::move(cb).finalize_to(cell_ref);
}

bool Anycast::cell_pack_anycast_info(Ref<vm::Cell>& cell_ref, int depth, Ref<td::BitString> rewrite_pfx) const {
  vm::CellBuilder cb;
  return pack_anycast_info(cb, depth, std::move(rewrite_pfx)) && std::move(cb).finalize_to(cell_ref);
}

bool Anycast::print_skip(PrettyPrinter& pp, vm::CellSlice& cs) const {
  int depth;
  return pp.open("anycast_info")
      && cs.fetch_uint_leq(30, depth)
      && pp.field_int(depth, "depth")
      && 1 <= depth
      && pp.fetch_bits_field(cs, depth, "rewrite_pfx")
      && pp.close();
}

bool Anycast::print_skip(JsonPrinter& pp, vm::CellSlice& cs) const {
  int depth;
  return pp.open("anycast_info")
      && cs.fetch_uint_leq(30, depth)
      && pp.field_int(depth, "depth")
      && 1 <= depth
      && pp.fetch_bits_field(cs, depth, "rewrite_pfx")
      && pp.close();
}

const Anycast t_Anycast;

//
// code for type `MsgAddressInt`
//
constexpr unsigned char MsgAddressInt::cons_tag[2];
constexpr const char* MsgAddressInt::cons_name[2];

int MsgAddressInt::check_tag(const vm::CellSlice& cs) const {
  switch (get_tag(cs)) {
  case addr_std:
    return cs.have(2) ? addr_std : -1;
  case addr_var:
    return cs.have(2) ? addr_var : -1;
  }
  return -1;
}

bool MsgAddressInt::skip(vm::CellSlice& cs) const {
  switch (get_tag(cs)) {
  case addr_std:
    return cs.advance(2)
        && t_Maybe_Anycast.skip(cs)
        && cs.advance(264);
  case addr_var: {
    int addr_len;
    return cs.advance(2)
        && t_Maybe_Anycast.skip(cs)
        && cs.fetch_uint_to(9, addr_len)
        && cs.advance(32)
        && cs.advance(addr_len);
    }
  }
  return false;
}

bool MsgAddressInt::validate_skip(int* ops, vm::CellSlice& cs, bool weak) const {
  switch (get_tag(cs)) {
  case addr_std:
    return cs.advance(2)
        && t_Maybe_Anycast.validate_skip(ops, cs, weak)
        && cs.advance(264);
  case addr_var: {
    int addr_len;
    return cs.advance(2)
        && t_Maybe_Anycast.validate_skip(ops, cs, weak)
        && cs.fetch_uint_to(9, addr_len)
        && cs.advance(32)
        && cs.advance(addr_len);
    }
  }
  return false;
}

bool MsgAddressInt::unpack(vm::CellSlice& cs, MsgAddressInt::Record_addr_std& data) const {
  return cs.fetch_ulong(2) == 2
      && t_Maybe_Anycast.fetch_to(cs, data.anycast)
      && cs.fetch_int_to(8, data.workchain_id)
      && cs.fetch_bits_to(data.address.bits(), 256);
}

bool MsgAddressInt::unpack_addr_std(vm::CellSlice& cs, Ref<CellSlice>& anycast, int& workchain_id, td::BitArray<256>& address) const {
  return cs.fetch_ulong(2) == 2
      && t_Maybe_Anycast.fetch_to(cs, anycast)
      && cs.fetch_int_to(8, workchain_id)
      && cs.fetch_bits_to(address.bits(), 256);
}

bool MsgAddressInt::cell_unpack(Ref<vm::Cell> cell_ref, MsgAddressInt::Record_addr_std& data) const {
  if (cell_ref.is_null()) { return false; }
  auto cs = load_cell_slice(std::move(cell_ref));
  return unpack(cs, data) && cs.empty_ext();
}

bool MsgAddressInt::cell_unpack_addr_std(Ref<vm::Cell> cell_ref, Ref<CellSlice>& anycast, int& workchain_id, td::BitArray<256>& address) const {
  if (cell_ref.is_null()) { return false; }
  auto cs = load_cell_slice(std::move(cell_ref));
  return unpack_addr_std(cs, anycast, workchain_id, address) && cs.empty_ext();
}

bool MsgAddressInt::unpack(vm::CellSlice& cs, MsgAddressInt::Record_addr_var& data) const {
  return cs.fetch_ulong(2) == 3
      && t_Maybe_Anycast.fetch_to(cs, data.anycast)
      && cs.fetch_uint_to(9, data.addr_len)
      && cs.fetch_int_to(32, data.workchain_id)
      && cs.fetch_bitstring_to(data.addr_len, data.address);
}

bool MsgAddressInt::cell_unpack(Ref<vm::Cell> cell_ref, MsgAddressInt::Record_addr_var& data) const {
  if (cell_ref.is_null()) { return false; }
  auto cs = load_cell_slice(std::move(cell_ref));
  return unpack(cs, data) && cs.empty_ext();
}

bool MsgAddressInt::pack(vm::CellBuilder& cb, const MsgAddressInt::Record_addr_std& data) const {
  return cb.store_long_bool(2, 2)
      && t_Maybe_Anycast.store_from(cb, data.anycast)
      && cb.store_long_rchk_bool(data.workchain_id, 8)
      && cb.store_bits_bool(data.address.cbits(), 256);
}

bool MsgAddressInt::pack_addr_std(vm::CellBuilder& cb, Ref<CellSlice> anycast, int workchain_id, td::BitArray<256> address) const {
  return cb.store_long_bool(2, 2)
      && t_Maybe_Anycast.store_from(cb, anycast)
      && cb.store_long_rchk_bool(workchain_id, 8)
      && cb.store_bits_bool(address.cbits(), 256);
}

bool MsgAddressInt::cell_pack(Ref<vm::Cell>& cell_ref, const MsgAddressInt::Record_addr_std& data) const {
  vm::CellBuilder cb;
  return pack(cb, data) && std::move(cb).finalize_to(cell_ref);
}

bool MsgAddressInt::cell_pack_addr_std(Ref<vm::Cell>& cell_ref, Ref<CellSlice> anycast, int workchain_id, td::BitArray<256> address) const {
  vm::CellBuilder cb;
  return pack_addr_std(cb, std::move(anycast), workchain_id, address) && std::move(cb).finalize_to(cell_ref);
}

bool MsgAddressInt::pack(vm::CellBuilder& cb, const MsgAddressInt::Record_addr_var& data) const {
  return cb.store_long_bool(3, 2)
      && t_Maybe_Anycast.store_from(cb, data.anycast)
      && cb.store_ulong_rchk_bool(data.addr_len, 9)
      && cb.store_long_rchk_bool(data.workchain_id, 32)
      && cb.append_bitstring_chk(data.address, data.addr_len);
}

bool MsgAddressInt::cell_pack(Ref<vm::Cell>& cell_ref, const MsgAddressInt::Record_addr_var& data) const {
  vm::CellBuilder cb;
  return pack(cb, data) && std::move(cb).finalize_to(cell_ref);
}

bool MsgAddressInt::print_skip(PrettyPrinter& pp, vm::CellSlice& cs) const {
  switch (get_tag(cs)) {
  case addr_std:
    return cs.advance(2)
        && pp.open("addr_std")
        && pp.field("anycast")
        && t_Maybe_Anycast.print_skip(pp, cs)
        && pp.fetch_int_field(cs, 8, "workchain_id")
        && pp.fetch_bits_field(cs, 256, "address")
        && pp.close();
  case addr_var: {
    int addr_len;
    return cs.advance(2)
        && pp.open("addr_var")
        && pp.field("anycast")
        && t_Maybe_Anycast.print_skip(pp, cs)
        && cs.fetch_uint_to(9, addr_len)
        && pp.field_int(addr_len, "addr_len")
        && pp.fetch_int_field(cs, 32, "workchain_id")
        && pp.fetch_bits_field(cs, addr_len, "address")
        && pp.close();
    }
  }
  return pp.fail("unknown constructor for MsgAddressInt");
}

bool MsgAddressInt::print_skip(JsonPrinter& pp, vm::CellSlice& cs) const {
  switch (get_tag(cs)) {
  case addr_std:
    return cs.advance(2)
        && pp.open("addr_std")
        && pp.field("anycast")
        && t_Maybe_Anycast.print_skip(pp, cs)
        && pp.fetch_int_field(cs, 8, "workchain_id")
        && pp.fetch_bits_field(cs, 256, "address")
        && pp.close();
  case addr_var: {
    int addr_len;
    return cs.advance(2)
        && pp.open("addr_var")
        && pp.field("anycast")
        && t_Maybe_Anycast.print_skip(pp, cs)
        && cs.fetch_uint_to(9, addr_len)
        && pp.field_int(addr_len, "addr_len")
        && pp.fetch_int_field(cs, 32, "workchain_id")
        && pp.fetch_bits_field(cs, addr_len, "address")
        && pp.close();
    }
  }
  return pp.fail("unknown constructor for MsgAddressInt");
}

const MsgAddressInt t_MsgAddressInt;

//
// code for type `MsgAddress`
//

int MsgAddress::check_tag(const vm::CellSlice& cs) const {
  switch (get_tag(cs)) {
  case cons1:
    return cons1;
  case cons2:
    return cons2;
  }
  return -1;
}

bool MsgAddress::skip(vm::CellSlice& cs) const {
  switch (get_tag(cs)) {
  case cons1:
    return t_MsgAddressInt.skip(cs);
  case cons2:
    return t_MsgAddressExt.skip(cs);
  }
  return false;
}

bool MsgAddress::validate_skip(int* ops, vm::CellSlice& cs, bool weak) const {
  switch (get_tag(cs)) {
  case cons1:
    return t_MsgAddressInt.validate_skip(ops, cs, weak);
  case cons2:
    return t_MsgAddressExt.validate_skip(ops, cs, weak);
  }
  return false;
}

bool MsgAddress::unpack(vm::CellSlice& cs, MsgAddress::Record_cons1& data) const {
  return t_MsgAddressInt.fetch_to(cs, data.x);
}

bool MsgAddress::unpack_cons1(vm::CellSlice& cs, Ref<CellSlice>& x) const {
  return t_MsgAddressInt.fetch_to(cs, x);
}

bool MsgAddress::cell_unpack(Ref<vm::Cell> cell_ref, MsgAddress::Record_cons1& data) const {
  if (cell_ref.is_null()) { return false; }
  auto cs = load_cell_slice(std::move(cell_ref));
  return unpack(cs, data) && cs.empty_ext();
}

bool MsgAddress::cell_unpack_cons1(Ref<vm::Cell> cell_ref, Ref<CellSlice>& x) const {
  if (cell_ref.is_null()) { return false; }
  auto cs = load_cell_slice(std::move(cell_ref));
  return unpack_cons1(cs, x) && cs.empty_ext();
}

bool MsgAddress::unpack(vm::CellSlice& cs, MsgAddress::Record_cons2& data) const {
  return t_MsgAddressExt.fetch_to(cs, data.x);
}

bool MsgAddress::unpack_cons2(vm::CellSlice& cs, Ref<CellSlice>& x) const {
  return t_MsgAddressExt.fetch_to(cs, x);
}

bool MsgAddress::cell_unpack(Ref<vm::Cell> cell_ref, MsgAddress::Record_cons2& data) const {
  if (cell_ref.is_null()) { return false; }
  auto cs = load_cell_slice(std::move(cell_ref));
  return unpack(cs, data) && cs.empty_ext();
}

bool MsgAddress::cell_unpack_cons2(Ref<vm::Cell> cell_ref, Ref<CellSlice>& x) const {
  if (cell_ref.is_null()) { return false; }
  auto cs = load_cell_slice(std::move(cell_ref));
  return unpack_cons2(cs, x) && cs.empty_ext();
}

bool MsgAddress::pack(vm::CellBuilder& cb, const MsgAddress::Record_cons1& data) const {
  return t_MsgAddressInt.store_from(cb, data.x);
}

bool MsgAddress::pack_cons1(vm::CellBuilder& cb, Ref<CellSlice> x) const {
  return t_MsgAddressInt.store_from(cb, x);
}

bool MsgAddress::cell_pack(Ref<vm::Cell>& cell_ref, const MsgAddress::Record_cons1& data) const {
  vm::CellBuilder cb;
  return pack(cb, data) && std::move(cb).finalize_to(cell_ref);
}

bool MsgAddress::cell_pack_cons1(Ref<vm::Cell>& cell_ref, Ref<CellSlice> x) const {
  vm::CellBuilder cb;
  return pack_cons1(cb, std::move(x)) && std::move(cb).finalize_to(cell_ref);
}

bool MsgAddress::pack(vm::CellBuilder& cb, const MsgAddress::Record_cons2& data) const {
  return t_MsgAddressExt.store_from(cb, data.x);
}

bool MsgAddress::pack_cons2(vm::CellBuilder& cb, Ref<CellSlice> x) const {
  return t_MsgAddressExt.store_from(cb, x);
}

bool MsgAddress::cell_pack(Ref<vm::Cell>& cell_ref, const MsgAddress::Record_cons2& data) const {
  vm::CellBuilder cb;
  return pack(cb, data) && std::move(cb).finalize_to(cell_ref);
}

bool MsgAddress::cell_pack_cons2(Ref<vm::Cell>& cell_ref, Ref<CellSlice> x) const {
  vm::CellBuilder cb;
  return pack_cons2(cb, std::move(x)) && std::move(cb).finalize_to(cell_ref);
}

bool MsgAddress::print_skip(PrettyPrinter& pp, vm::CellSlice& cs) const {
  switch (get_tag(cs)) {
  case cons1:
    return pp.open()
        && pp.field()
        && t_MsgAddressInt.print_skip(pp, cs)
        && pp.close();
  case cons2:
    return pp.open()
        && pp.field()
        && t_MsgAddressExt.print_skip(pp, cs)
        && pp.close();
  }
  return pp.fail("unknown constructor for MsgAddress");
}

bool MsgAddress::print_skip(JsonPrinter& pp, vm::CellSlice& cs) const {
  switch (get_tag(cs)) {
  case cons1:
    return pp.open()
        && pp.field()
        && t_MsgAddressInt.print_skip(pp, cs)
        && pp.close();
  case cons2:
    return pp.open()
        && pp.field()
        && t_MsgAddressExt.print_skip(pp, cs)
        && pp.close();
  }
  return pp.fail("unknown constructor for MsgAddress");
}

const MsgAddress t_MsgAddress;

//
// code for type `MchJettonTransfer`
//

int MchJettonTransfer::check_tag(const vm::CellSlice& cs) const {
  return jetton_transfer;
}

bool MchJettonTransfer::skip(vm::CellSlice& cs) const {
  return cs.advance(96)
      && t_VarUInteger_16.skip(cs)
      && t_MsgAddress.skip(cs)
      && t_MsgAddress.skip(cs)
      && t_Maybe_Ref_Cell.skip(cs)
      && t_VarUInteger_16.skip(cs);
}

bool MchJettonTransfer::validate_skip(int* ops, vm::CellSlice& cs, bool weak) const {
  return cs.advance(96)
      && t_VarUInteger_16.validate_skip(ops, cs, weak)
      && t_MsgAddress.validate_skip(ops, cs, weak)
      && t_MsgAddress.validate_skip(ops, cs, weak)
      && t_Maybe_Ref_Cell.validate_skip(ops, cs, weak)
      && t_VarUInteger_16.validate_skip(ops, cs, weak);
}

bool MchJettonTransfer::unpack(vm::CellSlice& cs, MchJettonTransfer::Record& data) const {
  return cs.fetch_uint_to(32, data.opcode)
      && cs.fetch_uint_to(64, data.query_id)
      && t_VarUInteger_16.fetch_to(cs, data.amount)
      && t_MsgAddress.fetch_to(cs, data.destination)
      && t_MsgAddress.fetch_to(cs, data.response)
      && t_Maybe_Ref_Cell.fetch_to(cs, data.custom_payload)
      && t_VarUInteger_16.fetch_to(cs, data.forward_amount);
}

bool MchJettonTransfer::cell_unpack(Ref<vm::Cell> cell_ref, MchJettonTransfer::Record& data) const {
  if (cell_ref.is_null()) { return false; }
  auto cs = load_cell_slice(std::move(cell_ref));
  return unpack(cs, data) && cs.empty_ext();
}

bool MchJettonTransfer::pack(vm::CellBuilder& cb, const MchJettonTransfer::Record& data) const {
  return cb.store_ulong_rchk_bool(data.opcode, 32)
      && cb.store_ulong_rchk_bool(data.query_id, 64)
      && t_VarUInteger_16.store_from(cb, data.amount)
      && t_MsgAddress.store_from(cb, data.destination)
      && t_MsgAddress.store_from(cb, data.response)
      && t_Maybe_Ref_Cell.store_from(cb, data.custom_payload)
      && t_VarUInteger_16.store_from(cb, data.forward_amount);
}

bool MchJettonTransfer::cell_pack(Ref<vm::Cell>& cell_ref, const MchJettonTransfer::Record& data) const {
  vm::CellBuilder cb;
  return pack(cb, data) && std::move(cb).finalize_to(cell_ref);
}

bool MchJettonTransfer::print_skip(PrettyPrinter& pp, vm::CellSlice& cs) const {
  return pp.open("jetton_transfer")
      && pp.fetch_uint_field(cs, 32, "opcode")
      && pp.fetch_uint_field(cs, 64, "query_id")
      && pp.field("amount")
      && t_VarUInteger_16.print_skip(pp, cs)
      && pp.field("destination")
      && t_MsgAddress.print_skip(pp, cs)
      && pp.field("response")
      && t_MsgAddress.print_skip(pp, cs)
      && pp.field("custom_payload")
      && t_Maybe_Ref_Cell.print_skip(pp, cs)
      && pp.field("forward_amount")
      && t_VarUInteger_16.print_skip(pp, cs)
      && pp.close();
}

bool MchJettonTransfer::print_skip(JsonPrinter& pp, vm::CellSlice& cs) const {
  return pp.open("jetton_transfer")
      && pp.fetch_uint_field(cs, 32, "opcode")
      && pp.fetch_uint_field(cs, 64, "query_id")
      && pp.field("amount")
      && t_VarUInteger_16.print_skip(pp, cs)
      && pp.field("destination")
      && t_MsgAddress.print_skip(pp, cs)
      && pp.field("response")
      && t_MsgAddress.print_skip(pp, cs)
      && pp.field("custom_payload")
      && t_Maybe_Ref_Cell.print_skip(pp, cs)
      && pp.field("forward_amount")
      && t_VarUInteger_16.print_skip(pp, cs)
      && pp.close();
}

const MchJettonTransfer t_MchJettonTransfer;

//
// code for type `MchJettonInternalTransfer`
//

int MchJettonInternalTransfer::check_tag(const vm::CellSlice& cs) const {
  return jetton_internal_transfer;
}

bool MchJettonInternalTransfer::skip(vm::CellSlice& cs) const {
  return cs.advance(96)
      && t_VarUInteger_16.skip(cs)
      && t_MsgAddress.skip(cs)
      && t_MsgAddress.skip(cs)
      && t_VarUInteger_16.skip(cs);
}

bool MchJettonInternalTransfer::validate_skip(int* ops, vm::CellSlice& cs, bool weak) const {
  return cs.advance(96)
      && t_VarUInteger_16.validate_skip(ops, cs, weak)
      && t_MsgAddress.validate_skip(ops, cs, weak)
      && t_MsgAddress.validate_skip(ops, cs, weak)
      && t_VarUInteger_16.validate_skip(ops, cs, weak);
}

bool MchJettonInternalTransfer::unpack(vm::CellSlice& cs, MchJettonInternalTransfer::Record& data) const {
  return cs.fetch_uint_to(32, data.opcode)
      && cs.fetch_uint_to(64, data.query_id)
      && t_VarUInteger_16.fetch_to(cs, data.amount)
      && t_MsgAddress.fetch_to(cs, data.from_address)
      && t_MsgAddress.fetch_to(cs, data.response_address)
      && t_VarUInteger_16.fetch_to(cs, data.forward_ton_amount);
}

bool MchJettonInternalTransfer::cell_unpack(Ref<vm::Cell> cell_ref, MchJettonInternalTransfer::Record& data) const {
  if (cell_ref.is_null()) { return false; }
  auto cs = load_cell_slice(std::move(cell_ref));
  return unpack(cs, data) && cs.empty_ext();
}

bool MchJettonInternalTransfer::pack(vm::CellBuilder& cb, const MchJettonInternalTransfer::Record& data) const {
  return cb.store_ulong_rchk_bool(data.opcode, 32)
      && cb.store_ulong_rchk_bool(data.query_id, 64)
      && t_VarUInteger_16.store_from(cb, data.amount)
      && t_MsgAddress.store_from(cb, data.from_address)
      && t_MsgAddress.store_from(cb, data.response_address)
      && t_VarUInteger_16.store_from(cb, data.forward_ton_amount);
}

bool MchJettonInternalTransfer::cell_pack(Ref<vm::Cell>& cell_ref, const MchJettonInternalTransfer::Record& data) const {
  vm::CellBuilder cb;
  return pack(cb, data) && std::move(cb).finalize_to(cell_ref);
}

bool MchJettonInternalTransfer::print_skip(PrettyPrinter& pp, vm::CellSlice& cs) const {
  return pp.open("jetton_internal_transfer")
      && pp.fetch_uint_field(cs, 32, "opcode")
      && pp.fetch_uint_field(cs, 64, "query_id")
      && pp.field("amount")
      && t_VarUInteger_16.print_skip(pp, cs)
      && pp.field("from_address")
      && t_MsgAddress.print_skip(pp, cs)
      && pp.field("response_address")
      && t_MsgAddress.print_skip(pp, cs)
      && pp.field("forward_ton_amount")
      && t_VarUInteger_16.print_skip(pp, cs)
      && pp.close();
}

bool MchJettonInternalTransfer::print_skip(JsonPrinter& pp, vm::CellSlice& cs) const {
  return pp.open("jetton_internal_transfer")
      && pp.fetch_uint_field(cs, 32, "opcode")
      && pp.fetch_uint_field(cs, 64, "query_id")
      && pp.field("amount")
      && t_VarUInteger_16.print_skip(pp, cs)
      && pp.field("from_address")
      && t_MsgAddress.print_skip(pp, cs)
      && pp.field("response_address")
      && t_MsgAddress.print_skip(pp, cs)
      && pp.field("forward_ton_amount")
      && t_VarUInteger_16.print_skip(pp, cs)
      && pp.close();
}

const MchJettonInternalTransfer t_MchJettonInternalTransfer;

//
// code for type `MchJettonBurn`
//

int MchJettonBurn::check_tag(const vm::CellSlice& cs) const {
  return jetton_burn;
}

bool MchJettonBurn::skip(vm::CellSlice& cs) const {
  return cs.advance(96)
      && t_VarUInteger_16.skip(cs)
      && t_MsgAddress.skip(cs);
}

bool MchJettonBurn::validate_skip(int* ops, vm::CellSlice& cs, bool weak) const {
  return cs.advance(96)
      && t_VarUInteger_16.validate_skip(ops, cs, weak)
      && t_MsgAddress.validate_skip(ops, cs, weak);
}

bool MchJettonBurn::unpack(vm::CellSlice& cs, MchJettonBurn::Record& data) const {
  return cs.fetch_uint_to(32, data.opcode)
      && cs.fetch_uint_to(64, data.query_id)
      && t_VarUInteger_16.fetch_to(cs, data.amount)
      && t_MsgAddress.fetch_to(cs, data.response_destination);
}

bool MchJettonBurn::cell_unpack(Ref<vm::Cell> cell_ref, MchJettonBurn::Record& data) const {
  if (cell_ref.is_null()) { return false; }
  auto cs = load_cell_slice(std::move(cell_ref));
  return unpack(cs, data) && cs.empty_ext();
}

bool MchJettonBurn::pack(vm::CellBuilder& cb, const MchJettonBurn::Record& data) const {
  return cb.store_ulong_rchk_bool(data.opcode, 32)
      && cb.store_ulong_rchk_bool(data.query_id, 64)
      && t_VarUInteger_16.store_from(cb, data.amount)
      && t_MsgAddress.store_from(cb, data.response_destination);
}

bool MchJettonBurn::cell_pack(Ref<vm::Cell>& cell_ref, const MchJettonBurn::Record& data) const {
  vm::CellBuilder cb;
  return pack(cb, data) && std::move(cb).finalize_to(cell_ref);
}

bool MchJettonBurn::print_skip(PrettyPrinter& pp, vm::CellSlice& cs) const {
  return pp.open("jetton_burn")
      && pp.fetch_uint_field(cs, 32, "opcode")
      && pp.fetch_uint_field(cs, 64, "query_id")
      && pp.field("amount")
      && t_VarUInteger_16.print_skip(pp, cs)
      && pp.field("response_destination")
      && t_MsgAddress.print_skip(pp, cs)
      && pp.close();
}

bool MchJettonBurn::print_skip(JsonPrinter& pp, vm::CellSlice& cs) const {
  return pp.open("jetton_burn")
      && pp.fetch_uint_field(cs, 32, "opcode")
      && pp.fetch_uint_field(cs, 64, "query_id")
      && pp.field("amount")
      && t_VarUInteger_16.print_skip(pp, cs)
      && pp.field("response_destination")
      && t_MsgAddress.print_skip(pp, cs)
      && pp.close();
}

const MchJettonBurn t_MchJettonBurn;

//
// code for type `MchJettonNotify`
//

int MchJettonNotify::check_tag(const vm::CellSlice& cs) const {
  return jetton_notify;
}

bool MchJettonNotify::skip(vm::CellSlice& cs) const {
  return cs.advance(96)
      && t_VarUInteger_16.skip(cs)
      && t_MsgAddress.skip(cs);
}

bool MchJettonNotify::validate_skip(int* ops, vm::CellSlice& cs, bool weak) const {
  return cs.advance(96)
      && t_VarUInteger_16.validate_skip(ops, cs, weak)
      && t_MsgAddress.validate_skip(ops, cs, weak);
}

bool MchJettonNotify::unpack(vm::CellSlice& cs, MchJettonNotify::Record& data) const {
  return cs.fetch_uint_to(32, data.opcode)
      && cs.fetch_uint_to(64, data.query_id)
      && t_VarUInteger_16.fetch_to(cs, data.jetton_amount)
      && t_MsgAddress.fetch_to(cs, data.from_user);
}

bool MchJettonNotify::cell_unpack(Ref<vm::Cell> cell_ref, MchJettonNotify::Record& data) const {
  if (cell_ref.is_null()) { return false; }
  auto cs = load_cell_slice(std::move(cell_ref));
  return unpack(cs, data) && cs.empty_ext();
}

bool MchJettonNotify::pack(vm::CellBuilder& cb, const MchJettonNotify::Record& data) const {
  return cb.store_ulong_rchk_bool(data.opcode, 32)
      && cb.store_ulong_rchk_bool(data.query_id, 64)
      && t_VarUInteger_16.store_from(cb, data.jetton_amount)
      && t_MsgAddress.store_from(cb, data.from_user);
}

bool MchJettonNotify::cell_pack(Ref<vm::Cell>& cell_ref, const MchJettonNotify::Record& data) const {
  vm::CellBuilder cb;
  return pack(cb, data) && std::move(cb).finalize_to(cell_ref);
}

bool MchJettonNotify::print_skip(PrettyPrinter& pp, vm::CellSlice& cs) const {
  return pp.open("jetton_notify")
      && pp.fetch_uint_field(cs, 32, "opcode")
      && pp.fetch_uint_field(cs, 64, "query_id")
      && pp.field("jetton_amount")
      && t_VarUInteger_16.print_skip(pp, cs)
      && pp.field("from_user")
      && t_MsgAddress.print_skip(pp, cs)
      && pp.close();
}

bool MchJettonNotify::print_skip(JsonPrinter& pp, vm::CellSlice& cs) const {
  return pp.open("jetton_notify")
      && pp.fetch_uint_field(cs, 32, "opcode")
      && pp.fetch_uint_field(cs, 64, "query_id")
      && pp.field("jetton_amount")
      && t_VarUInteger_16.print_skip(pp, cs)
      && pp.field("from_user")
      && t_MsgAddress.print_skip(pp, cs)
      && pp.close();
}

const MchJettonNotify t_MchJettonNotify;

//
// code for type `MchJettonMint`
//

int MchJettonMint::check_tag(const vm::CellSlice& cs) const {
  return jetton_mint;
}

bool MchJettonMint::skip(vm::CellSlice& cs) const {
  return cs.advance(96)
      && t_MsgAddress.skip(cs)
      && t_VarUInteger_16.skip(cs);
}

bool MchJettonMint::validate_skip(int* ops, vm::CellSlice& cs, bool weak) const {
  return cs.advance(96)
      && t_MsgAddress.validate_skip(ops, cs, weak)
      && t_VarUInteger_16.validate_skip(ops, cs, weak);
}

bool MchJettonMint::unpack(vm::CellSlice& cs, MchJettonMint::Record& data) const {
  return cs.fetch_uint_to(32, data.opcode)
      && cs.fetch_uint_to(64, data.query_id)
      && t_MsgAddress.fetch_to(cs, data.to_address)
      && t_VarUInteger_16.fetch_to(cs, data.ton_amount);
}

bool MchJettonMint::cell_unpack(Ref<vm::Cell> cell_ref, MchJettonMint::Record& data) const {
  if (cell_ref.is_null()) { return false; }
  auto cs = load_cell_slice(std::move(cell_ref));
  return unpack(cs, data) && cs.empty_ext();
}

bool MchJettonMint::pack(vm::CellBuilder& cb, const MchJettonMint::Record& data) const {
  return cb.store_ulong_rchk_bool(data.opcode, 32)
      && cb.store_ulong_rchk_bool(data.query_id, 64)
      && t_MsgAddress.store_from(cb, data.to_address)
      && t_VarUInteger_16.store_from(cb, data.ton_amount);
}

bool MchJettonMint::cell_pack(Ref<vm::Cell>& cell_ref, const MchJettonMint::Record& data) const {
  vm::CellBuilder cb;
  return pack(cb, data) && std::move(cb).finalize_to(cell_ref);
}

bool MchJettonMint::print_skip(PrettyPrinter& pp, vm::CellSlice& cs) const {
  return pp.open("jetton_mint")
      && pp.fetch_uint_field(cs, 32, "opcode")
      && pp.fetch_uint_field(cs, 64, "query_id")
      && pp.field("to_address")
      && t_MsgAddress.print_skip(pp, cs)
      && pp.field("ton_amount")
      && t_VarUInteger_16.print_skip(pp, cs)
      && pp.close();
}

bool MchJettonMint::print_skip(JsonPrinter& pp, vm::CellSlice& cs) const {
  return pp.open("jetton_mint")
      && pp.fetch_uint_field(cs, 32, "opcode")
      && pp.fetch_uint_field(cs, 64, "query_id")
      && pp.field("to_address")
      && t_MsgAddress.print_skip(pp, cs)
      && pp.field("ton_amount")
      && t_VarUInteger_16.print_skip(pp, cs)
      && pp.close();
}

const MchJettonMint t_MchJettonMint;

//
// code for type `MchMinterJettonMint`
//

int MchMinterJettonMint::check_tag(const vm::CellSlice& cs) const {
  return minter_jetton_mint;
}

bool MchMinterJettonMint::skip(vm::CellSlice& cs) const {
  return cs.advance(96)
      && t_MsgAddress.skip(cs)
      && t_VarUInteger_16.skip(cs)
      && cs.advance_refs(1);
}

bool MchMinterJettonMint::validate_skip(int* ops, vm::CellSlice& cs, bool weak) const {
  return cs.advance(96)
      && t_MsgAddress.validate_skip(ops, cs, weak)
      && t_VarUInteger_16.validate_skip(ops, cs, weak)
      && cs.advance_refs(1);
}

bool MchMinterJettonMint::unpack(vm::CellSlice& cs, MchMinterJettonMint::Record& data) const {
  return cs.fetch_uint_to(32, data.opcode)
      && cs.fetch_uint_to(64, data.query_id)
      && t_MsgAddress.fetch_to(cs, data.to_address)
      && t_VarUInteger_16.fetch_to(cs, data.ton_amount)
      && cs.fetch_ref_to(data.master_msg);
}

bool MchMinterJettonMint::cell_unpack(Ref<vm::Cell> cell_ref, MchMinterJettonMint::Record& data) const {
  if (cell_ref.is_null()) { return false; }
  auto cs = load_cell_slice(std::move(cell_ref));
  return unpack(cs, data) && cs.empty_ext();
}

bool MchMinterJettonMint::pack(vm::CellBuilder& cb, const MchMinterJettonMint::Record& data) const {
  return cb.store_ulong_rchk_bool(data.opcode, 32)
      && cb.store_ulong_rchk_bool(data.query_id, 64)
      && t_MsgAddress.store_from(cb, data.to_address)
      && t_VarUInteger_16.store_from(cb, data.ton_amount)
      && cb.store_ref_bool(data.master_msg);
}

bool MchMinterJettonMint::cell_pack(Ref<vm::Cell>& cell_ref, const MchMinterJettonMint::Record& data) const {
  vm::CellBuilder cb;
  return pack(cb, data) && std::move(cb).finalize_to(cell_ref);
}

bool MchMinterJettonMint::print_skip(PrettyPrinter& pp, vm::CellSlice& cs) const {
  return pp.open("minter_jetton_mint")
      && pp.fetch_uint_field(cs, 32, "opcode")
      && pp.fetch_uint_field(cs, 64, "query_id")
      && pp.field("to_address")
      && t_MsgAddress.print_skip(pp, cs)
      && pp.field("ton_amount")
      && t_VarUInteger_16.print_skip(pp, cs)
      && pp.field("master_msg")
      && t_Anything.print_ref(pp, cs.fetch_ref())
      && pp.close();
}

bool MchMinterJettonMint::print_skip(JsonPrinter& pp, vm::CellSlice& cs) const {
  return pp.open("minter_jetton_mint")
      && pp.fetch_uint_field(cs, 32, "opcode")
      && pp.fetch_uint_field(cs, 64, "query_id")
      && pp.field("to_address")
      && t_MsgAddress.print_skip(pp, cs)
      && pp.field("ton_amount")
      && t_VarUInteger_16.print_skip(pp, cs)
      && pp.field("master_msg")
      && t_Anything.print_ref(pp, cs.fetch_ref())
      && pp.close();
}

const MchMinterJettonMint t_MchMinterJettonMint;

// definitions of constant types used

const NatWidth t_natwidth_9{9};
const NatLeq t_natleq_30{30};
const Maybe t_Maybe_Anycast{t_Anycast};
const Int t_int8{8};
const Bits t_bits256{256};
const Int t_int32{32};
const UInt t_uint32{32};
const UInt t_uint64{64};
const VarUInteger t_VarUInteger_16{16};
const Maybe t_Maybe_Ref_Cell{t_RefCell};

// definition of type name registration function
bool register_simple_types(std::function<bool(const char*, const TLB*)> func) {
  return func("MsgAddressExt", &t_MsgAddressExt)
      && func("Anycast", &t_Anycast)
      && func("MsgAddressInt", &t_MsgAddressInt)
      && func("MsgAddress", &t_MsgAddress)
      && func("MchJettonTransfer", &t_MchJettonTransfer)
      && func("MchJettonInternalTransfer", &t_MchJettonInternalTransfer)
      && func("MchJettonBurn", &t_MchJettonBurn)
      && func("MchJettonNotify", &t_MchJettonNotify)
      && func("MchJettonMint", &t_MchJettonMint)
      && func("MchMinterJettonMint", &t_MchMinterJettonMint);
}


} // namespace gen

} // namespace mchmsg
