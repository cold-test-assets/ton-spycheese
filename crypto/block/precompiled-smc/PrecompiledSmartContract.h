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
#include "common/refcnt.hpp"
#include "common/refint.h"
#include "vm/cells.h"
#include "vm/cellslice.h"
#include "vm/dict.h"
#include "vm/boc.h"
#include <ostream>
#include "tl/tlblib.hpp"
#include "td/utils/bits.h"
#include "ton/ton-types.h"
#include "block/block.h"
#include "block/mc-config.h"

namespace block::precompiled {

class PrecompiledSmartContract {
 public:
  struct Result {
    int exit_code = 0;
    td::optional<long long> exit_arg;
    bool accepted = true;
    bool committed = false;

    static Result error(int code, long long arg = 0) {
      Result res;
      res.exit_code = code;
      res.exit_arg = arg;
      return res;
    }

    static Result error(vm::Excno code, long long arg = 0) {
      Result res;
      res.exit_code = (int)code;
      res.exit_arg = arg;
      return res;
    }

    static Result not_accepted(int code = 0) {
      Result res;
      res.exit_code = code;
      res.accepted = false;
      return res;
    }

    static Result success() {
      Result res;
      res.committed = true;
      return res;
    }
  };

  virtual ~PrecompiledSmartContract() = default;

  virtual std::string get_name() const = 0;

  Result run(td::Ref<vm::CellSlice> my_address, ton::UnixTime now, CurrencyCollection balance, td::Ref<vm::Cell> c4,
             vm::CellSlice msg_body, td::Ref<vm::Cell> msg, CurrencyCollection msg_balance, bool is_external) {
    my_address_ = std::move(my_address);
    now_ = now;
    balance_ = std::move(balance);
    c4_ = std::move(c4);
    msg_body_ = std::move(msg_body);
    msg_ = std::move(msg);
    msg_balance_ = std::move(msg_balance);
    is_external_ = is_external;
    try {
      return do_run();
    } catch (vm::VmError &e) {
      return Result::error(e.get_errno(), e.get_arg());
    } catch (Result &r) {
      return r;
    }
  }

  td::Ref<vm::Cell> get_c4() const {
    return c4_;
  }
  td::Ref<vm::Cell> get_c5() const {
    return c5_;
  }

 protected:
  td::Ref<vm::CellSlice> my_address_;
  ton::UnixTime now_;
  CurrencyCollection balance_;
  vm::CellSlice msg_body_;
  td::Ref<vm::Cell> msg_;
  CurrencyCollection msg_balance_;
  bool is_external_;

  td::Ref<vm::Cell> c4_;
  td::Ref<vm::Cell> c5_ = vm::CellBuilder().finalize_novm();

  virtual Result do_run() = 0;
};

std::unique_ptr<PrecompiledSmartContract> get_precompiled_smart_contract(td::Bits256 code_hash);

}  // namespace block::precompiled