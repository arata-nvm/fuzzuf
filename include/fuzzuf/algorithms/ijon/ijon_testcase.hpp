/*
 * fuzzuf
 * Copyright (C) 2021-2023 Ricerca Security
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Affero General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Affero General Public License for more details.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with this program.  If not, see http://www.gnu.org/licenses/.
 */

#ifndef FUZZUF_INCLUDE_ALGORITHM_IJON_IJON_TESTCASE_HPP
#define FUZZUF_INCLUDE_ALGORITHM_IJON_IJON_TESTCASE_HPP

#include <memory>

#include "fuzzuf/algorithms/afl/afl_testcase.hpp"
#include "fuzzuf/algorithms/ijon/ijon_option.hpp"
#include "fuzzuf/exec_input/on_disk_exec_input.hpp"

namespace fuzzuf::algorithm::ijon {

/**
 * @struct
 * Just an alias of AFLTestcase(with Tag replaced)
 */
struct IJONTestcase : public afl::AFLTestcase {
  using Tag = option::IJONTag;

  explicit IJONTestcase(std::shared_ptr<exec_input::OnDiskExecInput> input);
  ~IJONTestcase();
};

}  // namespace fuzzuf::algorithm::ijon

#endif
