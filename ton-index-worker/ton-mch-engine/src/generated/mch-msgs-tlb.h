#pragma once
#include <tl/tlblib.hpp>
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
// headers for type `Maybe`
//

struct Maybe final : TLB_Complex {
  using TLB::print_skip;
  enum { nothing, just };
  static constexpr int cons_len_exact = 1;
  const TLB &X_;
  Maybe(const TLB& X) : X_(X) {}
  struct Record_nothing {
    typedef Maybe type_class;
  };
  struct Record_just {
    typedef Maybe type_class;
    Ref<CellSlice> value;  	// value : X
    Record_just() = default;
    Record_just(Ref<CellSlice> _value) : value(std::move(_value)) {}
  };
  bool skip(vm::CellSlice& cs) const override;
  bool validate_skip(int* ops, vm::CellSlice& cs, bool weak = false) const override;
  bool unpack(vm::CellSlice& cs, Record_nothing& data) const;
  bool unpack_nothing(vm::CellSlice& cs) const;
  bool cell_unpack(Ref<vm::Cell> cell_ref, Record_nothing& data) const;
  bool cell_unpack_nothing(Ref<vm::Cell> cell_ref) const;
  bool pack(vm::CellBuilder& cb, const Record_nothing& data) const;
  bool pack_nothing(vm::CellBuilder& cb) const;
  bool cell_pack(Ref<vm::Cell>& cell_ref, const Record_nothing& data) const;
  bool cell_pack_nothing(Ref<vm::Cell>& cell_ref) const;
  bool unpack(vm::CellSlice& cs, Record_just& data) const;
  bool unpack_just(vm::CellSlice& cs, Ref<CellSlice>& value) const;
  bool cell_unpack(Ref<vm::Cell> cell_ref, Record_just& data) const;
  bool cell_unpack_just(Ref<vm::Cell> cell_ref, Ref<CellSlice>& value) const;
  bool pack(vm::CellBuilder& cb, const Record_just& data) const;
  bool pack_just(vm::CellBuilder& cb, Ref<CellSlice> value) const;
  bool cell_pack(Ref<vm::Cell>& cell_ref, const Record_just& data) const;
  bool cell_pack_just(Ref<vm::Cell>& cell_ref, Ref<CellSlice> value) const;
  bool print_skip(PrettyPrinter& pp, vm::CellSlice& cs) const override;
  bool print_skip(JsonPrinter& pp, vm::CellSlice& cs) const override;
  std::ostream& print_type(std::ostream& os) const override {
    return os << "(Maybe " << X_ << ")";
  }
  int check_tag(const vm::CellSlice& cs) const override;
  int get_tag(const vm::CellSlice& cs) const override {
    return (int)cs.prefetch_ulong(1);
  }
};

//
// headers for type `VarUInteger`
//

struct VarUInteger final : TLB_Complex {
  using TLB::print_skip;
  enum { var_uint };
  static constexpr int cons_len_exact = 0;
  int m_;
  VarUInteger(int m) : m_(m) {}
  struct Record;
  bool skip(vm::CellSlice& cs) const override;
  bool validate_skip(int* ops, vm::CellSlice& cs, bool weak = false) const override;
  bool unpack(vm::CellSlice& cs, Record& data) const;
  bool unpack_var_uint(vm::CellSlice& cs, int& n, int& len, RefInt256& value) const;
  bool cell_unpack(Ref<vm::Cell> cell_ref, Record& data) const;
  bool cell_unpack_var_uint(Ref<vm::Cell> cell_ref, int& n, int& len, RefInt256& value) const;
  bool pack(vm::CellBuilder& cb, const Record& data) const;
  bool pack_var_uint(vm::CellBuilder& cb, int len, RefInt256 value) const;
  bool cell_pack(Ref<vm::Cell>& cell_ref, const Record& data) const;
  bool cell_pack_var_uint(Ref<vm::Cell>& cell_ref, int len, RefInt256 value) const;
  bool print_skip(PrettyPrinter& pp, vm::CellSlice& cs) const override;
  bool print_skip(JsonPrinter& pp, vm::CellSlice& cs) const override;
  std::ostream& print_type(std::ostream& os) const override {
    return os << "(VarUInteger " << m_ << ")";
  }
  int check_tag(const vm::CellSlice& cs) const override;
  int get_tag(const vm::CellSlice& cs) const override {
    return 0;
  }
};

struct VarUInteger::Record {
  typedef VarUInteger type_class;
  int n;  	// n : #
  int len;  	// len : #< n
  RefInt256 value;  	// value : uint (8 * len)
  Record() = default;
  Record(int _len, RefInt256 _value) : n(-1), len(_len), value(std::move(_value)) {}
};

//
// headers for type `MsgAddressExt`
//

struct MsgAddressExt final : TLB_Complex {
  using TLB::print_skip;
  enum { addr_none, addr_extern };
  static constexpr int cons_len_exact = 2;
  struct Record_addr_none {
    typedef MsgAddressExt type_class;
  };
  struct Record_addr_extern {
    typedef MsgAddressExt type_class;
    int len;  	// len : ## 9
    Ref<td::BitString> external_address;  	// external_address : bits len
    Record_addr_extern() = default;
    Record_addr_extern(int _len, Ref<td::BitString> _external_address) : len(_len), external_address(std::move(_external_address)) {}
  };
  bool skip(vm::CellSlice& cs) const override;
  bool validate_skip(int* ops, vm::CellSlice& cs, bool weak = false) const override;
  bool unpack(vm::CellSlice& cs, Record_addr_none& data) const;
  bool unpack_addr_none(vm::CellSlice& cs) const;
  bool cell_unpack(Ref<vm::Cell> cell_ref, Record_addr_none& data) const;
  bool cell_unpack_addr_none(Ref<vm::Cell> cell_ref) const;
  bool pack(vm::CellBuilder& cb, const Record_addr_none& data) const;
  bool pack_addr_none(vm::CellBuilder& cb) const;
  bool cell_pack(Ref<vm::Cell>& cell_ref, const Record_addr_none& data) const;
  bool cell_pack_addr_none(Ref<vm::Cell>& cell_ref) const;
  bool unpack(vm::CellSlice& cs, Record_addr_extern& data) const;
  bool unpack_addr_extern(vm::CellSlice& cs, int& len, Ref<td::BitString>& external_address) const;
  bool cell_unpack(Ref<vm::Cell> cell_ref, Record_addr_extern& data) const;
  bool cell_unpack_addr_extern(Ref<vm::Cell> cell_ref, int& len, Ref<td::BitString>& external_address) const;
  bool pack(vm::CellBuilder& cb, const Record_addr_extern& data) const;
  bool pack_addr_extern(vm::CellBuilder& cb, int len, Ref<td::BitString> external_address) const;
  bool cell_pack(Ref<vm::Cell>& cell_ref, const Record_addr_extern& data) const;
  bool cell_pack_addr_extern(Ref<vm::Cell>& cell_ref, int len, Ref<td::BitString> external_address) const;
  bool print_skip(PrettyPrinter& pp, vm::CellSlice& cs) const override;
  bool print_skip(JsonPrinter& pp, vm::CellSlice& cs) const override;
  std::ostream& print_type(std::ostream& os) const override {
    return os << "MsgAddressExt";
  }
  int check_tag(const vm::CellSlice& cs) const override;
  int get_tag(const vm::CellSlice& cs) const override {
    return cs.bselect(2, 3);
  }
};

extern const MsgAddressExt t_MsgAddressExt;

//
// headers for type `Anycast`
//

struct Anycast final : TLB_Complex {
  using TLB::print_skip;
  enum { anycast_info };
  static constexpr int cons_len_exact = 0;
  struct Record {
    typedef Anycast type_class;
    int depth;  	// depth : #<= 30
    Ref<td::BitString> rewrite_pfx;  	// rewrite_pfx : bits depth
    Record() = default;
    Record(int _depth, Ref<td::BitString> _rewrite_pfx) : depth(_depth), rewrite_pfx(std::move(_rewrite_pfx)) {}
  };
  bool skip(vm::CellSlice& cs) const override;
  bool validate_skip(int* ops, vm::CellSlice& cs, bool weak = false) const override;
  bool unpack(vm::CellSlice& cs, Record& data) const;
  bool unpack_anycast_info(vm::CellSlice& cs, int& depth, Ref<td::BitString>& rewrite_pfx) const;
  bool cell_unpack(Ref<vm::Cell> cell_ref, Record& data) const;
  bool cell_unpack_anycast_info(Ref<vm::Cell> cell_ref, int& depth, Ref<td::BitString>& rewrite_pfx) const;
  bool pack(vm::CellBuilder& cb, const Record& data) const;
  bool pack_anycast_info(vm::CellBuilder& cb, int depth, Ref<td::BitString> rewrite_pfx) const;
  bool cell_pack(Ref<vm::Cell>& cell_ref, const Record& data) const;
  bool cell_pack_anycast_info(Ref<vm::Cell>& cell_ref, int depth, Ref<td::BitString> rewrite_pfx) const;
  bool print_skip(PrettyPrinter& pp, vm::CellSlice& cs) const override;
  bool print_skip(JsonPrinter& pp, vm::CellSlice& cs) const override;
  std::ostream& print_type(std::ostream& os) const override {
    return os << "Anycast";
  }
  int check_tag(const vm::CellSlice& cs) const override;
  int get_tag(const vm::CellSlice& cs) const override {
    return 0;
  }
};

extern const Anycast t_Anycast;

//
// headers for type `MsgAddressInt`
//

struct MsgAddressInt final : TLB_Complex {
  using TLB::print_skip;
  enum { addr_std, addr_var };
  static constexpr int cons_len_exact = 2;
  static constexpr unsigned char cons_tag[2] = { 2, 3 };
  static constexpr const char* cons_name[2] = { "addr_std", "addr_var" };
  struct Record_addr_std;
  struct Record_addr_var;
  bool skip(vm::CellSlice& cs) const override;
  bool validate_skip(int* ops, vm::CellSlice& cs, bool weak = false) const override;
  bool unpack(vm::CellSlice& cs, Record_addr_std& data) const;
  bool unpack_addr_std(vm::CellSlice& cs, Ref<CellSlice>& anycast, int& workchain_id, td::BitArray<256>& address) const;
  bool cell_unpack(Ref<vm::Cell> cell_ref, Record_addr_std& data) const;
  bool cell_unpack_addr_std(Ref<vm::Cell> cell_ref, Ref<CellSlice>& anycast, int& workchain_id, td::BitArray<256>& address) const;
  bool pack(vm::CellBuilder& cb, const Record_addr_std& data) const;
  bool pack_addr_std(vm::CellBuilder& cb, Ref<CellSlice> anycast, int workchain_id, td::BitArray<256> address) const;
  bool cell_pack(Ref<vm::Cell>& cell_ref, const Record_addr_std& data) const;
  bool cell_pack_addr_std(Ref<vm::Cell>& cell_ref, Ref<CellSlice> anycast, int workchain_id, td::BitArray<256> address) const;
  bool unpack(vm::CellSlice& cs, Record_addr_var& data) const;
  bool cell_unpack(Ref<vm::Cell> cell_ref, Record_addr_var& data) const;
  bool pack(vm::CellBuilder& cb, const Record_addr_var& data) const;
  bool cell_pack(Ref<vm::Cell>& cell_ref, const Record_addr_var& data) const;
  bool print_skip(PrettyPrinter& pp, vm::CellSlice& cs) const override;
  bool print_skip(JsonPrinter& pp, vm::CellSlice& cs) const override;
  std::ostream& print_type(std::ostream& os) const override {
    return os << "MsgAddressInt";
  }
  int check_tag(const vm::CellSlice& cs) const override;
  int get_tag(const vm::CellSlice& cs) const override {
    return cs.bselect(2, 12);
  }
};

struct MsgAddressInt::Record_addr_std {
  typedef MsgAddressInt type_class;
  Ref<CellSlice> anycast;  	// anycast : Maybe Anycast
  int workchain_id;  	// workchain_id : int8
  td::BitArray<256> address;  	// address : bits256
  Record_addr_std() = default;
  Record_addr_std(Ref<CellSlice> _anycast, int _workchain_id, const td::BitArray<256>& _address) : anycast(std::move(_anycast)), workchain_id(_workchain_id), address(_address) {}
};

struct MsgAddressInt::Record_addr_var {
  typedef MsgAddressInt type_class;
  Ref<CellSlice> anycast;  	// anycast : Maybe Anycast
  int addr_len;  	// addr_len : ## 9
  int workchain_id;  	// workchain_id : int32
  Ref<td::BitString> address;  	// address : bits addr_len
  Record_addr_var() = default;
  Record_addr_var(Ref<CellSlice> _anycast, int _addr_len, int _workchain_id, Ref<td::BitString> _address) : anycast(std::move(_anycast)), addr_len(_addr_len), workchain_id(_workchain_id), address(std::move(_address)) {}
};

extern const MsgAddressInt t_MsgAddressInt;

//
// headers for type `MsgAddress`
//

struct MsgAddress final : TLB_Complex {
  using TLB::print_skip;
  enum { cons2, cons1 };
  static constexpr int cons_len_exact = 0;
  struct Record_cons1 {
    typedef MsgAddress type_class;
    Ref<CellSlice> x;  	// MsgAddressInt
    Record_cons1() = default;
    Record_cons1(Ref<CellSlice> _x) : x(std::move(_x)) {}
  };
  struct Record_cons2 {
    typedef MsgAddress type_class;
    Ref<CellSlice> x;  	// MsgAddressExt
    Record_cons2() = default;
    Record_cons2(Ref<CellSlice> _x) : x(std::move(_x)) {}
  };
  bool skip(vm::CellSlice& cs) const override;
  bool validate_skip(int* ops, vm::CellSlice& cs, bool weak = false) const override;
  bool unpack(vm::CellSlice& cs, Record_cons1& data) const;
  bool unpack_cons1(vm::CellSlice& cs, Ref<CellSlice>& x) const;
  bool cell_unpack(Ref<vm::Cell> cell_ref, Record_cons1& data) const;
  bool cell_unpack_cons1(Ref<vm::Cell> cell_ref, Ref<CellSlice>& x) const;
  bool pack(vm::CellBuilder& cb, const Record_cons1& data) const;
  bool pack_cons1(vm::CellBuilder& cb, Ref<CellSlice> x) const;
  bool cell_pack(Ref<vm::Cell>& cell_ref, const Record_cons1& data) const;
  bool cell_pack_cons1(Ref<vm::Cell>& cell_ref, Ref<CellSlice> x) const;
  bool unpack(vm::CellSlice& cs, Record_cons2& data) const;
  bool unpack_cons2(vm::CellSlice& cs, Ref<CellSlice>& x) const;
  bool cell_unpack(Ref<vm::Cell> cell_ref, Record_cons2& data) const;
  bool cell_unpack_cons2(Ref<vm::Cell> cell_ref, Ref<CellSlice>& x) const;
  bool pack(vm::CellBuilder& cb, const Record_cons2& data) const;
  bool pack_cons2(vm::CellBuilder& cb, Ref<CellSlice> x) const;
  bool cell_pack(Ref<vm::Cell>& cell_ref, const Record_cons2& data) const;
  bool cell_pack_cons2(Ref<vm::Cell>& cell_ref, Ref<CellSlice> x) const;
  bool print_skip(PrettyPrinter& pp, vm::CellSlice& cs) const override;
  bool print_skip(JsonPrinter& pp, vm::CellSlice& cs) const override;
  std::ostream& print_type(std::ostream& os) const override {
    return os << "MsgAddress";
  }
  int check_tag(const vm::CellSlice& cs) const override;
  int get_tag(const vm::CellSlice& cs) const override {
    return (int)cs.prefetch_ulong(1);
  }
};

extern const MsgAddress t_MsgAddress;

//
// headers for type `MchJettonTransfer`
//

struct MchJettonTransfer final : TLB_Complex {
  using TLB::print_skip;
  enum { jetton_transfer };
  static constexpr int cons_len_exact = 0;
  struct Record;
  bool skip(vm::CellSlice& cs) const override;
  bool validate_skip(int* ops, vm::CellSlice& cs, bool weak = false) const override;
  bool unpack(vm::CellSlice& cs, Record& data) const;
  bool cell_unpack(Ref<vm::Cell> cell_ref, Record& data) const;
  bool pack(vm::CellBuilder& cb, const Record& data) const;
  bool cell_pack(Ref<vm::Cell>& cell_ref, const Record& data) const;
  bool print_skip(PrettyPrinter& pp, vm::CellSlice& cs) const override;
  bool print_skip(JsonPrinter& pp, vm::CellSlice& cs) const override;
  std::ostream& print_type(std::ostream& os) const override {
    return os << "MchJettonTransfer";
  }
  int check_tag(const vm::CellSlice& cs) const override;
  int get_tag(const vm::CellSlice& cs) const override {
    return 0;
  }
};

struct MchJettonTransfer::Record {
  typedef MchJettonTransfer type_class;
  unsigned opcode;  	// opcode : uint32
  unsigned long long query_id;  	// query_id : uint64
  Ref<CellSlice> amount;  	// amount : VarUInteger 16
  Ref<CellSlice> destination;  	// destination : MsgAddress
  Ref<CellSlice> response;  	// response : MsgAddress
  Ref<CellSlice> custom_payload;  	// custom_payload : Maybe ^Cell
  Ref<CellSlice> forward_amount;  	// forward_amount : VarUInteger 16
  Record() = default;
  Record(unsigned _opcode, unsigned long long _query_id, Ref<CellSlice> _amount, Ref<CellSlice> _destination, Ref<CellSlice> _response, Ref<CellSlice> _custom_payload, Ref<CellSlice> _forward_amount) : opcode(_opcode), query_id(_query_id), amount(std::move(_amount)), destination(std::move(_destination)), response(std::move(_response)), custom_payload(std::move(_custom_payload)), forward_amount(std::move(_forward_amount)) {}
};

extern const MchJettonTransfer t_MchJettonTransfer;

//
// headers for type `MchJettonInternalTransfer`
//

struct MchJettonInternalTransfer final : TLB_Complex {
  using TLB::print_skip;
  enum { jetton_internal_transfer };
  static constexpr int cons_len_exact = 0;
  struct Record;
  bool skip(vm::CellSlice& cs) const override;
  bool validate_skip(int* ops, vm::CellSlice& cs, bool weak = false) const override;
  bool unpack(vm::CellSlice& cs, Record& data) const;
  bool cell_unpack(Ref<vm::Cell> cell_ref, Record& data) const;
  bool pack(vm::CellBuilder& cb, const Record& data) const;
  bool cell_pack(Ref<vm::Cell>& cell_ref, const Record& data) const;
  bool print_skip(PrettyPrinter& pp, vm::CellSlice& cs) const override;
  bool print_skip(JsonPrinter& pp, vm::CellSlice& cs) const override;
  std::ostream& print_type(std::ostream& os) const override {
    return os << "MchJettonInternalTransfer";
  }
  int check_tag(const vm::CellSlice& cs) const override;
  int get_tag(const vm::CellSlice& cs) const override {
    return 0;
  }
};

struct MchJettonInternalTransfer::Record {
  typedef MchJettonInternalTransfer type_class;
  unsigned opcode;  	// opcode : uint32
  unsigned long long query_id;  	// query_id : uint64
  Ref<CellSlice> amount;  	// amount : VarUInteger 16
  Ref<CellSlice> from_address;  	// from_address : MsgAddress
  Ref<CellSlice> response_address;  	// response_address : MsgAddress
  Ref<CellSlice> forward_ton_amount;  	// forward_ton_amount : VarUInteger 16
  Record() = default;
  Record(unsigned _opcode, unsigned long long _query_id, Ref<CellSlice> _amount, Ref<CellSlice> _from_address, Ref<CellSlice> _response_address, Ref<CellSlice> _forward_ton_amount) : opcode(_opcode), query_id(_query_id), amount(std::move(_amount)), from_address(std::move(_from_address)), response_address(std::move(_response_address)), forward_ton_amount(std::move(_forward_ton_amount)) {}
};

extern const MchJettonInternalTransfer t_MchJettonInternalTransfer;

//
// headers for type `MchJettonBurn`
//

struct MchJettonBurn final : TLB_Complex {
  using TLB::print_skip;
  enum { jetton_burn };
  static constexpr int cons_len_exact = 0;
  struct Record;
  bool skip(vm::CellSlice& cs) const override;
  bool validate_skip(int* ops, vm::CellSlice& cs, bool weak = false) const override;
  bool unpack(vm::CellSlice& cs, Record& data) const;
  bool cell_unpack(Ref<vm::Cell> cell_ref, Record& data) const;
  bool pack(vm::CellBuilder& cb, const Record& data) const;
  bool cell_pack(Ref<vm::Cell>& cell_ref, const Record& data) const;
  bool print_skip(PrettyPrinter& pp, vm::CellSlice& cs) const override;
  bool print_skip(JsonPrinter& pp, vm::CellSlice& cs) const override;
  std::ostream& print_type(std::ostream& os) const override {
    return os << "MchJettonBurn";
  }
  int check_tag(const vm::CellSlice& cs) const override;
  int get_tag(const vm::CellSlice& cs) const override {
    return 0;
  }
};

struct MchJettonBurn::Record {
  typedef MchJettonBurn type_class;
  unsigned opcode;  	// opcode : uint32
  unsigned long long query_id;  	// query_id : uint64
  Ref<CellSlice> amount;  	// amount : VarUInteger 16
  Ref<CellSlice> response_destination;  	// response_destination : MsgAddress
  Record() = default;
  Record(unsigned _opcode, unsigned long long _query_id, Ref<CellSlice> _amount, Ref<CellSlice> _response_destination) : opcode(_opcode), query_id(_query_id), amount(std::move(_amount)), response_destination(std::move(_response_destination)) {}
};

extern const MchJettonBurn t_MchJettonBurn;

//
// headers for type `MchJettonNotify`
//

struct MchJettonNotify final : TLB_Complex {
  using TLB::print_skip;
  enum { jetton_notify };
  static constexpr int cons_len_exact = 0;
  struct Record;
  bool skip(vm::CellSlice& cs) const override;
  bool validate_skip(int* ops, vm::CellSlice& cs, bool weak = false) const override;
  bool unpack(vm::CellSlice& cs, Record& data) const;
  bool cell_unpack(Ref<vm::Cell> cell_ref, Record& data) const;
  bool pack(vm::CellBuilder& cb, const Record& data) const;
  bool cell_pack(Ref<vm::Cell>& cell_ref, const Record& data) const;
  bool print_skip(PrettyPrinter& pp, vm::CellSlice& cs) const override;
  bool print_skip(JsonPrinter& pp, vm::CellSlice& cs) const override;
  std::ostream& print_type(std::ostream& os) const override {
    return os << "MchJettonNotify";
  }
  int check_tag(const vm::CellSlice& cs) const override;
  int get_tag(const vm::CellSlice& cs) const override {
    return 0;
  }
};

struct MchJettonNotify::Record {
  typedef MchJettonNotify type_class;
  unsigned opcode;  	// opcode : uint32
  unsigned long long query_id;  	// query_id : uint64
  Ref<CellSlice> jetton_amount;  	// jetton_amount : VarUInteger 16
  Ref<CellSlice> from_user;  	// from_user : MsgAddress
  Record() = default;
  Record(unsigned _opcode, unsigned long long _query_id, Ref<CellSlice> _jetton_amount, Ref<CellSlice> _from_user) : opcode(_opcode), query_id(_query_id), jetton_amount(std::move(_jetton_amount)), from_user(std::move(_from_user)) {}
};

extern const MchJettonNotify t_MchJettonNotify;

//
// headers for type `MchJettonMint`
//

struct MchJettonMint final : TLB_Complex {
  using TLB::print_skip;
  enum { jetton_mint };
  static constexpr int cons_len_exact = 0;
  struct Record;
  bool skip(vm::CellSlice& cs) const override;
  bool validate_skip(int* ops, vm::CellSlice& cs, bool weak = false) const override;
  bool unpack(vm::CellSlice& cs, Record& data) const;
  bool cell_unpack(Ref<vm::Cell> cell_ref, Record& data) const;
  bool pack(vm::CellBuilder& cb, const Record& data) const;
  bool cell_pack(Ref<vm::Cell>& cell_ref, const Record& data) const;
  bool print_skip(PrettyPrinter& pp, vm::CellSlice& cs) const override;
  bool print_skip(JsonPrinter& pp, vm::CellSlice& cs) const override;
  std::ostream& print_type(std::ostream& os) const override {
    return os << "MchJettonMint";
  }
  int check_tag(const vm::CellSlice& cs) const override;
  int get_tag(const vm::CellSlice& cs) const override {
    return 0;
  }
};

struct MchJettonMint::Record {
  typedef MchJettonMint type_class;
  unsigned opcode;  	// opcode : uint32
  unsigned long long query_id;  	// query_id : uint64
  Ref<CellSlice> to_address;  	// to_address : MsgAddress
  Ref<CellSlice> ton_amount;  	// ton_amount : VarUInteger 16
  Record() = default;
  Record(unsigned _opcode, unsigned long long _query_id, Ref<CellSlice> _to_address, Ref<CellSlice> _ton_amount) : opcode(_opcode), query_id(_query_id), to_address(std::move(_to_address)), ton_amount(std::move(_ton_amount)) {}
};

extern const MchJettonMint t_MchJettonMint;

//
// headers for type `MchMinterJettonMint`
//

struct MchMinterJettonMint final : TLB_Complex {
  using TLB::print_skip;
  enum { minter_jetton_mint };
  static constexpr int cons_len_exact = 0;
  struct Record;
  bool skip(vm::CellSlice& cs) const override;
  bool validate_skip(int* ops, vm::CellSlice& cs, bool weak = false) const override;
  bool unpack(vm::CellSlice& cs, Record& data) const;
  bool cell_unpack(Ref<vm::Cell> cell_ref, Record& data) const;
  bool pack(vm::CellBuilder& cb, const Record& data) const;
  bool cell_pack(Ref<vm::Cell>& cell_ref, const Record& data) const;
  bool print_skip(PrettyPrinter& pp, vm::CellSlice& cs) const override;
  bool print_skip(JsonPrinter& pp, vm::CellSlice& cs) const override;
  std::ostream& print_type(std::ostream& os) const override {
    return os << "MchMinterJettonMint";
  }
  int check_tag(const vm::CellSlice& cs) const override;
  int get_tag(const vm::CellSlice& cs) const override {
    return 0;
  }
};

struct MchMinterJettonMint::Record {
  typedef MchMinterJettonMint type_class;
  unsigned opcode;  	// opcode : uint32
  unsigned long long query_id;  	// query_id : uint64
  Ref<CellSlice> to_address;  	// to_address : MsgAddress
  Ref<CellSlice> ton_amount;  	// ton_amount : VarUInteger 16
  Ref<Cell> master_msg;  	// master_msg : ^Cell
  Record() = default;
  Record(unsigned _opcode, unsigned long long _query_id, Ref<CellSlice> _to_address, Ref<CellSlice> _ton_amount, Ref<Cell> _master_msg) : opcode(_opcode), query_id(_query_id), to_address(std::move(_to_address)), ton_amount(std::move(_ton_amount)), master_msg(std::move(_master_msg)) {}
};

extern const MchMinterJettonMint t_MchMinterJettonMint;

// declarations of constant types used

// ## 9
extern const NatWidth t_natwidth_9;
// #<= 30
extern const NatLeq t_natleq_30;
// Maybe Anycast
extern const Maybe t_Maybe_Anycast;
// int8
extern const Int t_int8;
// bits256
extern const Bits t_bits256;
// int32
extern const Int t_int32;
// uint32
extern const UInt t_uint32;
// uint64
extern const UInt t_uint64;
// VarUInteger 16
extern const VarUInteger t_VarUInteger_16;
// Maybe ^Cell
extern const Maybe t_Maybe_Ref_Cell;

// declaration of type name registration function
extern bool register_simple_types(std::function<bool(const char*, const TLB*)> func);

} // namespace gen

} // namespace mchmsg
