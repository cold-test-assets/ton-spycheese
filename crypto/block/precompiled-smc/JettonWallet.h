/*
    This file is part of TON Blockchain Library.

    TON Blockchain Library is free software: you can redistribute it and/or modify
    it under the terms of the GNU Lesser General Public License as published by
    the Free Software Foundation, either version 2 of the License, or
    (at your option) any later version.

    TON Blockchain Library is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU Lesser General Public License for more details.

    You should have received a copy of the GNU Lesser General Public License
    along with TON Blockchain Library.  If not, see <http://www.gnu.org/licenses/>.
*/
#pragma once
#include "PrecompiledSmartContract.h"

namespace block::precompiled {

// FunC code: https://github.com/ton-blockchain/token-contract/blob/991bdb4925653c51b0b53ab212c53143f71f5476/ft/jetton-wallet.fc

using namespace vm;

class JettonWallet : public PrecompiledSmartContract {
 public:
  std::string get_name() const override {
    return "jetton-wallet";
  }

 protected:
  static bool skip_maybe_anycast(CellSlice& cs) {
    if (cs.prefetch_ulong(1) != 1) {
      return cs.advance(1);
    }
    unsigned depth;
    return cs.advance(1)                    // just$1
           && cs.fetch_uint_leq(30, depth)  // anycast_info$_ depth:(#<= 30)
           && depth >= 1                    // { depth >= 1 }
           && cs.advance(depth);            // rewrite_pfx:(bits depth) = Anycast;
  }

  static bool skip_message_addr(CellSlice& cs) {
    switch ((unsigned)cs.fetch_ulong(2)) {
      case 0:  // addr_none$00 = MsgAddressExt;
        return true;
      case 1: {  // addr_extern$01
        unsigned len;
        return cs.fetch_uint_to(9, len)  // len:(## 9)
               && cs.advance(len);       // external_address:(bits len) = MsgAddressExt;
      }
      case 2: {                         // addr_std$10
        return skip_maybe_anycast(cs)   // anycast:(Maybe Anycast)
               && cs.advance(8 + 256);  // workchain_id:int8 address:bits256  = MsgAddressInt;
      }
      case 3: {  // addr_var$11
        unsigned len;
        return skip_maybe_anycast(cs)       // anycast:(Maybe Anycast)
               && cs.fetch_uint_to(9, len)  // addr_len:(## 9)
               && cs.advance(32 + len);     // workchain_id:int32 address:(bits addr_len) = MsgAddressInt;
      }
      default:
        return false;
    }
  }
  static CellSlice load_msg_addr(CellSlice& cs) {
    CellSlice cs0 = cs;
    if (!skip_message_addr(cs)) {
      throw VmError{Excno::cell_und, "cannot load a MsgAddress"};
    }
    return *cs0.prefetch_subslice(cs0.size() - cs.size());
  }
  static td::RefInt256 load_coins(CellSlice& cs) {
    td::RefInt256 x;
    int len;
    if (!(cs.fetch_uint_to(4, len) && cs.fetch_int256_to(len * 8, x, false))) {
      throw VmError{Excno::cell_und, "cannot deserialize a variable-length integer"};
    }
    return x;
  }

  static void store_coins(CellBuilder& cb, const td::RefInt256& x) {
    unsigned len = (((unsigned)x->bit_size(false) + 7) >> 3);
    if (len >= 16) {
      throw VmError{Excno::range_chk};
    }
    if (!(cb.store_long_bool(len, 4) && cb.store_int256_bool(*x, len * 8, false))) {
      throw VmError{Excno::cell_ov, "cannot serialize a variable-length integer"};
    }
  }

  static void store_long(CellBuilder& cb, long long x, unsigned bits) {
    if (bits > 64) {
      if (!cb.store_zeroes_bool(bits - 64)) {
        throw VmError{Excno::cell_ov};
      }
      bits = 64;
    }
    if (!cb.store_long_bool(x, bits)) {
      throw VmError{Excno::cell_ov};
    }
  }

  static void store_slice(CellBuilder& cb, const CellSlice& cs) {
    if (!cb.append_cellslice_bool(cs)) {
      throw VmError{Excno::cell_ov};
    }
  }

  static void store_ref(CellBuilder& cb, const td::Ref<Cell>& ref) {
    if (!cb.store_ref_bool(ref)) {
      throw VmError{Excno::cell_ov};
    }
  }

  static void store_maybe_ref(CellBuilder& cb, const td::Ref<Cell>& ref) {
    if (!cb.store_maybe_ref(ref)) {
      throw VmError{Excno::cell_ov};
    }
  }

  void send_raw_message(const td::Ref<Cell>& msg, int mode) {
    CellBuilder cb;
    if (!(cb.store_ref_bool(c5_)                 // out_list$_ {n:#} prev:^(OutList n)
          && cb.store_long_bool(0x0ec3c86d, 32)  // action_send_msg#0ec3c86d
          && cb.store_long_bool(mode, 8)         // mode:(## 8)
          && cb.store_ref_bool(msg))) {
      throw VmError{Excno::cell_ov, "cannot serialize raw output message into an output action cell"};
    }
    c5_ = cb.finalize_novm();
  }

  Result do_run() override {
    if (is_external_) {
      return Result::not_accepted();
    }
    if (msg_body_.empty_ext()) {  // Ignore empty messages
      return Result::success();
    }
    CellSlice cs = load_cell_slice(msg_);
    auto flags = cs.fetch_long(4);
    if (flags & 1) {
      on_bounce();
      return Result::success();
    }
    sender_address_ = load_msg_addr(cs);
    load_msg_addr(cs);  // skip dst
    load_coins(cs);     // skip value
    cs.skip_first(1);   // skip extracurrency collection
    load_coins(cs);     // skip ihr_fee
    td::RefInt256 fwd_fee = load_coins(cs);
    fwd_fee = (fwd_fee * 3) >> 1;  // we use message fwd_fee for estimation of forward_payload costs

    td::uint32 op;
    if (!msg_body_.fetch_uint_to(32, op)) {
      return Result::error(Excno::cell_und);
    }

    if (op == OP_TRANSFER) {  // outgoing transfer
      send_tokens(fwd_fee);
      return Result::success();
    }

    if (op == OP_INTERNAL_TRANSFER) {  // incoming transfer
      receive_tokens(fwd_fee);
      return Result::success();
    }

    if (op == OP_BURN) {  // burn
      burn_tokens(fwd_fee);
      return Result::success();
    }

    return Result::error(0xffff);
  }

  void on_bounce() {
    if (!msg_body_.skip_first(32)) {  // 0xFFFFFFFF
      throw VmError{Excno::cell_und};
    }
    load_data();
    td::uint32 op;
    if (!msg_body_.fetch_uint_to(32, op)) {
      throw VmError{Excno::cell_und};
    }
    if (op != OP_INTERNAL_TRANSFER && op != OP_BURN_NOTIFICATION) {
      throw Result::error(709);
    }
    td::uint64 query_id;
    if (!msg_body_.fetch_uint_to(64, query_id)) {
      throw VmError{Excno::cell_und};
    }
    td::RefInt256 jetton_amount = load_coins(msg_body_);
    data_balance_ += jetton_amount;
    if (!data_balance_->signed_fits_bits(257)) {
      throw VmError{Excno::int_ov};
    }
    save_data();
  }

  void send_tokens(td::RefInt256 fwd_fee) {
    td::uint64 query_id;
    if (!msg_body_.fetch_uint_to(64, query_id)) {
      throw VmError{Excno::cell_und};
    }
    td::RefInt256 jetton_amount = load_coins(msg_body_);
    CellSlice to_owner_address = load_msg_addr(msg_body_);
    force_chain(to_owner_address);
    load_data();
    data_balance_ -= jetton_amount;

    if (data_owner_.lex_cmp(sender_address_)) {
      throw Result::error(705);
    }
    if (data_balance_->sgn() < 0) {
      throw Result::error(706);
    }

    auto state_init = calculate_jetton_wallet_state_init(to_owner_address);
    auto to_wallet_address = calculate_jetton_wallet_address(state_init);
    auto response_address = load_msg_addr(msg_body_);
    td::Ref<Cell> custom_payload;
    if (!msg_body_.fetch_maybe_ref(custom_payload)) {
      throw VmError{Excno::cell_und};
    }
    td::RefInt256 forward_ton_amount = load_coins(msg_body_);
    if (msg_body_.size() == 0) {
      throw Result::error(708);
    }
    CellBuilder cb_msg;
    store_long(cb_msg, 0x18, 6);
    store_slice(cb_msg, to_wallet_address);
    store_long(cb_msg, 4 + 2 + 1, 4 + 1 + 4 + 4 + 64 + 32 + 1 + 1 + 1);
    store_ref(cb_msg, state_init);
    CellBuilder cb_msg_body;
    store_long(cb_msg_body, OP_INTERNAL_TRANSFER, 32);
    store_long(cb_msg_body, query_id, 64);
    store_coins(cb_msg_body, jetton_amount);
    store_slice(cb_msg_body, data_owner_);
    store_slice(cb_msg_body, response_address);
    store_coins(cb_msg_body, forward_ton_amount);
    store_slice(cb_msg_body, msg_body_);
    store_ref(cb_msg, cb_msg_body.finalize_novm());
    int fwd_count = forward_ton_amount->sgn() ? 2 : 1;
    // 3 messages: wal1->wal2,  wal2->owner, wal2->response
    // but last one is optional (it is ok if it fails)
    if (msg_balance_.grams <= forward_ton_amount + fwd_fee * fwd_count + (2 * GAS_CONSUMPTION + MIN_TONS_FOR_STORAGE)) {
      throw Result::error(709);
    }
    // universal message send fee calculation may be activated here
    // by using this instead of fwd_fee
    // msg_fwd_fee(to_wallet, msg_body, state_init, 15)
    send_raw_message(cb_msg.finalize_novm(), 64);  // revert on errors
    save_data();
  }

  void receive_tokens(td::RefInt256 fwd_fee) {
    // NOTE we can not allow fails in action phase since in that case there will be
    // no bounce. Thus check and throw in computation phase.
    load_data();
    td::uint64 query_id;
    if (!msg_body_.fetch_uint_to(64, query_id)) {
      throw VmError{Excno::cell_und};
    }
    td::RefInt256 jetton_amount = load_coins(msg_body_);
    data_balance_ += jetton_amount;
    if (!data_balance_->signed_fits_bits(257)) {
      throw VmError{Excno::int_ov};
    }
    CellSlice from_address = load_msg_addr(msg_body_);
    CellSlice response_address = load_msg_addr(msg_body_);
    if (sender_address_.lex_cmp(data_jetton_master_) &&
        sender_address_.lex_cmp(calculate_user_jetton_wallet_address(from_address))) {
      throw Result::error(707);
    }
    td::RefInt256 forward_ton_amount = load_coins(msg_body_);
    td::RefInt256 ton_balance_before_msg = balance_.grams - msg_balance_.grams;
    td::RefInt256 storage_fee =
        std::max(td::zero_refint(), td::make_refint(MIN_TONS_FOR_STORAGE) - ton_balance_before_msg);
    td::RefInt256 msg_value = msg_balance_.grams - (storage_fee + GAS_CONSUMPTION);
    if (forward_ton_amount->sgn()) {
      msg_value -= (forward_ton_amount + fwd_fee);
      CellBuilder cb_msg_body;
      store_long(cb_msg_body, OP_TRANSFER_NOTIFICATION, 32);
      store_long(cb_msg_body, query_id, 64);
      store_coins(cb_msg_body, jetton_amount);
      store_slice(cb_msg_body, from_address);
      store_slice(cb_msg_body, msg_body_);

      CellBuilder cb_msg;
      store_long(cb_msg, 0x10, 6);  // we should not bounce here cause receiver can have uninitialized contract
      store_slice(cb_msg, data_owner_);
      store_coins(cb_msg, forward_ton_amount);
      store_long(cb_msg, 1, 1 + 4 + 4 + 64 + 32 + 1 + 1);
      store_ref(cb_msg, cb_msg_body.finalize_novm());
      send_raw_message(cb_msg.finalize_novm(), 1);
    }

    if (response_address.prefetch_long(2) != 0 && msg_value->sgn() > 0) {
      CellBuilder cb_msg;
      store_long(cb_msg, 0x10,
                 6);  // nobounce - int_msg_info$0 ihr_disabled:Bool bounce:Bool bounced:Bool src:MsgAddress -> 010000
      store_slice(cb_msg, response_address);
      store_coins(cb_msg, msg_value);
      store_long(cb_msg, 0, 1 + 4 + 4 + 64 + 32 + 1 + 1);
      store_long(cb_msg, OP_EXCESSES, 32);
      store_long(cb_msg, query_id, 64);
      send_raw_message(cb_msg.finalize_novm(), 2);
    }

    save_data();
  }

  void burn_tokens(td::RefInt256 fwd_fee) {
    // NOTE we can not allow fails in action phase since in that case there will be
    // no bounce. Thus check and throw in computation phase.
    load_data();
    td::uint64 query_id;
    if (!msg_body_.fetch_uint_to(64, query_id)) {
      throw VmError{Excno::cell_und};
    }
    td::RefInt256 jetton_amount = load_coins(msg_body_);
    CellSlice response_address = load_msg_addr(msg_body_);
    // ignore custom payload
    // slice custom_payload = in_msg_body~load_dict();
    data_balance_ -= jetton_amount;
    if (data_owner_.lex_cmp(sender_address_)) {
      throw Result::error(705);
    }
    if (data_balance_->sgn() < 0) {
      throw Result::error(706);
    }
    if (msg_balance_.grams <= fwd_fee + 2 * GAS_CONSUMPTION) {
      throw Result::error(707);
    }

    CellBuilder cb_msg_body;
    store_long(cb_msg_body, OP_BURN_NOTIFICATION, 32);
    store_long(cb_msg_body, query_id, 64);
    store_coins(cb_msg_body, jetton_amount);
    store_slice(cb_msg_body, data_owner_);
    store_slice(cb_msg_body, response_address);

    CellBuilder cb_msg;
    store_long(cb_msg, 0x18, 6);
    store_slice(cb_msg, data_jetton_master_);
    store_long(cb_msg, 1, 4 + 1 + 4 + 4 + 64 + 32 + 1 + 1);
    store_ref(cb_msg, cb_msg_body.finalize_novm());

    send_raw_message(cb_msg.finalize_novm(), 64);

    save_data();
  }

  void load_data() {
    CellSlice ds = load_cell_slice(c4_);
    data_balance_ = load_coins(ds);
    data_owner_ = load_msg_addr(ds);
    data_jetton_master_ = load_msg_addr(ds);
    data_jetton_wallet_code_ = ds.prefetch_ref(0);
    if (data_jetton_wallet_code_.is_null()) {
      throw VmError{Excno::cell_und};
    }
  }

  void save_data() {
    c4_ = pack_jetton_wallet_data(data_balance_, data_owner_);
  }

  void force_chain(CellSlice addr) {
    auto p = parse_std_addr(std::move(addr));
    if (p.first != WORKCHAIN) {
      throw Result::error(333);
    }
  }

  static bool parse_maybe_anycast(CellSlice& cs, td::Ref<CellSlice>& res) {
    res = {};
    if (cs.prefetch_ulong(1) != 1) {
      return cs.advance(1);
    }
    unsigned depth;
    Ref<CellSlice> pfx;
    if (cs.advance(1)                           // just$1
        && cs.fetch_uint_leq(30, depth)         // anycast_info$_ depth:(#<= 30)
        && depth >= 1                           // { depth >= 1 }
        && cs.fetch_subslice_to(depth, pfx)) {  // rewrite_pfx:(bits depth) = Anycast;
      res = std::move(pfx);
      return true;
    }
    return false;
  }

  static std::pair<int, td::Bits256> parse_std_addr(CellSlice cs) {
    if (!cs.have(2)) {
      throw VmError{Excno::cell_und};
    }
    td::Ref<CellSlice> anycast;
    int workchain, len = 256;
    td::Bits256 addr;
    auto tag = (unsigned)cs.fetch_ulong(2);
    bool ok = false;
    if (tag == 2) {
      ok = parse_maybe_anycast(cs, anycast) && cs.fetch_int_to(8, workchain) && cs.fetch_bits_to(addr);
    } else if (tag == 3) {
      ok = parse_maybe_anycast(cs, anycast) && cs.fetch_uint_to(9, len) && (len == 256) &&
           cs.fetch_int_to(32, workchain) && cs.fetch_bits_to(addr);
    }
    if (!ok) {
      throw VmError{Excno::cell_und, "cannot parse a MsgAddressInt"};
    }
    CHECK(anycast.is_null() || anycast->prefetch_bits_to(addr.bits(), anycast->size()));
    return {workchain, addr};
  }

  td::Ref<Cell> pack_jetton_wallet_data(const td::RefInt256& balance, const CellSlice& owner_address) {
    CellBuilder cb;
    store_coins(cb, balance);
    store_slice(cb, owner_address);
    store_slice(cb, data_jetton_master_);
    store_ref(cb, data_jetton_wallet_code_);
    return cb.finalize_novm();
  }

  td::Ref<Cell> calculate_jetton_wallet_state_init(const CellSlice& owner_address) {
    CellBuilder cb;
    store_long(cb, 0, 2);
    store_maybe_ref(cb, data_jetton_wallet_code_);
    store_maybe_ref(cb, pack_jetton_wallet_data(td::zero_refint(), owner_address));
    store_long(cb, 0, 1);
    return cb.finalize_novm();
  }

  CellSlice calculate_jetton_wallet_address(const td::Ref<Cell>& state_init) {
    CellBuilder cb;
    cb.store_long(4, 3);
    cb.store_long(WORKCHAIN, 8);
    cb.store_bits(state_init->get_hash().bits(), 256);
    return load_cell_slice(cb.finalize_novm());
  }

  CellSlice calculate_user_jetton_wallet_address(const CellSlice& owner_address) {
    return calculate_jetton_wallet_address(calculate_jetton_wallet_state_init(owner_address));
  }

  CellSlice sender_address_;

  td::RefInt256 data_balance_;
  CellSlice data_owner_;
  CellSlice data_jetton_master_;
  td::Ref<Cell> data_jetton_wallet_code_;

  static const int WORKCHAIN = 0;
  static const td::uint32 OP_TRANSFER = 0xf8a7ea5;
  static const td::uint32 OP_TRANSFER_NOTIFICATION = 0x7362d09c;
  static const td::uint32 OP_INTERNAL_TRANSFER = 0x178d4519;
  static const td::uint32 OP_EXCESSES = 0xd53276db;
  static const td::uint32 OP_BURN = 0x595f07bc;
  static const td::uint32 OP_BURN_NOTIFICATION = 0x7bdd97de;

  static const long long MIN_TONS_FOR_STORAGE = 10000000;  // 0.01 TON
  static const long long GAS_CONSUMPTION = 15000000;       // 0.015 TON
};

}  // namespace block::precompiled
