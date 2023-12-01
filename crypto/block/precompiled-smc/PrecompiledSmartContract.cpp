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
#include "PrecompiledSmartContract.h"
#include "JettonWallet.h"
#include <memory>

namespace block::precompiled {

std::unique_ptr<PrecompiledSmartContract> get_precompiled_smart_contract(td::Bits256 code_hash) {
  static std::map<td::Bits256, std::unique_ptr<PrecompiledSmartContract> (*)()> map = []() {
    auto from_hex = [](td::Slice s) -> td::Bits256 {
      td::Bits256 x;
      CHECK(x.from_hex(s) == 256);
      return x;
    };
    std::map<td::Bits256, std::unique_ptr<PrecompiledSmartContract> (*)()> map;
#define CONTRACT(hash, cls) \
  map[from_hex(hash)] = []() -> std::unique_ptr<PrecompiledSmartContract> { return std::make_unique<cls>(); };
    CONTRACT("A760D629D5343E76D045017D9DC216FC8A307A8377815FEB2B0A5C490E733486", JettonWallet);
    CONTRACT("9E06CA30E508A6D9FB21167CBB8F66399EDD1190B0B01E0BBD14C546CC97A72B", JettonWallet);
    return map;
  }();
  auto it = map.find(code_hash);
  return it == map.end() ? nullptr : it->second();
}

}  // namespace block::precompiled
