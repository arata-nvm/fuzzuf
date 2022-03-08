/*
 * fuzzuf
 * Copyright (C) 2022 Ricerca Security
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

/**
 * @file fuzzuf_bb_cov_attacher.hpp
 * @author Ricerca Security <fuzzuf-dev@ricsec.co.jp>
 */

#ifndef FUZZUF_INCLUDE_COVERAGE_FUZZUF_BB_COV_ATTACHER_HPP
#define FUZZUF_INCLUDE_COVERAGE_FUZZUF_BB_COV_ATTACHER_HPP

#include "fuzzuf/coverage/shm_cov_attacher.hpp"

/**
 * @class FuzzufBBCovAttacher
 * @brief fuzzuf-specific basic block coverage attacher.
 */
class FuzzufBBCovAttacher : public ShmCovAttacher {
public:
  // FIXME: we have to modify fuzzuf-cc to change __WYVERN_SHM_ID to __FUZZUF_SHM_ID
  static constexpr const char* SHM_ENV_VAR = "__WYVERN_SHM_ID";

  FuzzufBBCovAttacher(u32 map_size) : ShmCovAttacher(map_size) {}
  void SetupEnvironmentVariable(void) {
    ShmCovAttacher::SetupEnvironmentVariable(SHM_ENV_VAR);
  }
};

#endif // FUZZUF_INCLUDE_COVERAGE_FUZZUF_BB_COV_ATTACHER_HPP