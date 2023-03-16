#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <iostream>
#include <nlohmann/json.hpp>
#include <boost/spirit/include/qi.hpp>
#include <fuzzuf/algorithms/eclipser/core/executor.hpp>
#include <fuzzuf/algorithms/eclipser/core/seed.hpp>
#include <fuzzuf/algorithms/eclipser/core/utils.hpp>
#include <fuzzuf/algorithms/eclipser/core/options.hpp>
#include <fuzzuf/algorithms/eclipser/core/bytes_utils.hpp>
#include <fuzzuf/algorithms/eclipser/core/failwith.hpp>
#include <fuzzuf/algorithms/eclipser/gray_concolic/solve.hpp>
#include <fuzzuf/algorithms/eclipser/gray_concolic/branch_tree.hpp>
#include <fuzzuf/algorithms/eclipser/gray_concolic/linear_equation.hpp>
#include <fuzzuf/algorithms/eclipser/gray_concolic/monotonicity.hpp>
#include <fuzzuf/algorithms/eclipser/gray_concolic/path_constraint.hpp>
#include <fuzzuf/utils/map_file.hpp>

namespace fuzzuf::algorithm::eclipser::gray_concolic {

namespace gray_solver {
std::optional< BigInt >
FindNextCharAux(
  const std::function<void(std::string &&)> &sink,
  const seed::Seed &seed,
  const options::FuzzOption &opt,
  const BranchPoint &targ_pt,
  const std::vector< std::byte > &acc_str,
  const std::vector< BranchInfo > &acc_br_infos,
  const std::vector< BigInt >::const_iterator &try_vals_begin,
  const std::vector< BigInt >::const_iterator &try_vals_end
) {
  if( try_vals_begin == try_vals_end ) {
    return std::nullopt;
  }
  const auto try_val = *try_vals_begin;
  std::vector< std::byte > try_str = acc_str;
  try_str.push_back( std::byte( std::uint8_t( try_val ) ) );
  const auto try_seed = seed.FixCurBytes( Direction::Right, try_str );
  const auto br_info = executor::GetBranchInfoOnly( sink, opt, try_seed, try_val, targ_pt );
  if( !br_info ) { // Failed to observe target point, proceed with the next tryVal.
    return FindNextCharAux(
      sink,
      seed,
      opt,
      targ_pt,
      acc_str,
      acc_br_infos,
      std::next( try_vals_begin ),
      try_vals_end
    );
  }
  else {
    auto new_acc_br_infos = acc_br_infos;
    new_acc_br_infos.push_back( *br_info );
    const auto lin_eq = branch_tree::InferLinEq(
      Context{},
      new_acc_br_infos.begin(),
      new_acc_br_infos.end()
    );
    if( !lin_eq ) { // No linear equation found yet, proceed with more brInfo.
      return FindNextCharAux(
        sink,
        seed,
        opt,
        targ_pt,
        acc_str,
        new_acc_br_infos,
        std::next( try_vals_begin ),
        try_vals_end
      );
    }
    // The solution of this equation is next character.
    if( lin_eq->solutions.empty() ) {
      failwith( "Linear equation w/ empty solution" );
    }
    return *lin_eq->solutions.begin();
  }
}

namespace {

std::optional< BigInt >
FindNextChar(
  const std::function<void(std::string &&)> &sink,
  const seed::Seed &seed,
  const options::FuzzOption &opt,
  const BranchPoint &targ_pt,
  const std::vector< std::byte > &acc_str
) {
  const auto sample_vals = SampleInt( 0, 255, opt.n_spawn );
  return FindNextCharAux(
    sink,
    seed,
    opt,
    targ_pt,
    acc_str,
    {},
    sample_vals.begin(),
    sample_vals.end()
  );
}

}

std::vector< std::tuple< seed::Seed, Signal, CoverageGain > >
TryStrSol(
  const std::function<void(std::string &&)> &sink,
  const seed::Seed &seed,
  const options::FuzzOption &opt,
  std::size_t max_len,
  const BranchPoint &targ_pt,
  const std::vector< std::tuple< seed::Seed, Signal, CoverageGain > > &acc_res,
  const std::vector< std::byte > &try_str
) {
  const auto try_seed = seed.FixCurBytes( Direction::Right, try_str );
    // Use dummy value as 'tryVal', since our interest is in branch distance.
  const auto [exit_sig,cov_gain,br_info] = executor::GetBranchInfo( sink, opt, try_seed, 0, targ_pt );
  if( br_info && br_info->distance == 0 ) {
    std::vector< std::tuple< seed::Seed, Signal, CoverageGain > > new_acc_res;
    new_acc_res.reserve( acc_res.size() + 1u );
    new_acc_res.push_back( std::make_tuple( try_seed, exit_sig, cov_gain ) );
    new_acc_res.insert(
      new_acc_res.end(),
      acc_res.begin(),
      acc_res.end()
    );
    return new_acc_res;
  }
  else if( br_info ) { // Non-zero branch distance, try next character.
    if( try_str.size() >= max_len ) {
      return acc_res;
    }
    else {
      const auto next_char_opt = FindNextChar( sink, seed, opt, targ_pt, try_str );
      if( !next_char_opt ) {
        return acc_res;
      }
      else {
        auto new_try_str = try_str;
        new_try_str.push_back( std::byte( std::uint8_t( *next_char_opt ) ) );
        return TryStrSol( sink, seed, opt, max_len, targ_pt, acc_res, new_try_str );
      }
    }
  }
  else {
    return acc_res; // Target point disappeared, halt.
  }
}
std::vector< std::tuple< seed::Seed, Signal, CoverageGain > >
SolveAsString(
  const std::function<void(std::string &&)> &sink,
  const seed::Seed &seed,
  const options::FuzzOption &opt,
  const BranchPoint &targ_pt,
  const LinearEquation &lin_eq,
  const std::vector< std::tuple< seed::Seed, Signal, CoverageGain > > &acc_res
) {
  std::vector< std::vector< std::byte > > init_strs;
  init_strs.reserve( lin_eq.solutions.size() );
  std::transform(
    lin_eq.solutions.begin(),
    lin_eq.solutions.end(),
    std::back_inserter( init_strs ),
    [&]( const auto &v ) {
      return BigIntToBytes( Endian::BE, 1u, v );
    }
  );
  const auto max_len = seed.QueryUpdateBound( Direction::Right );
  auto new_acc_res = acc_res;
  for( const auto &v: init_strs ) {
    new_acc_res = TryStrSol( sink, seed, opt, max_len, targ_pt, new_acc_res, v );
  }
  return new_acc_res;
}

std::unordered_set< BigInt > solution_cache;

void ClearSolutionCache() {
  solution_cache.clear();
}

std::vector< std::tuple< seed::Seed, Signal, CoverageGain > >
TryChunkSol(
  const std::function<void(std::string &&)> &sink,
  const seed::Seed &seed,
  const options::FuzzOption &opt,
  Direction dir,
  const BranchPoint &targ_pt,
  Endian endian,
  std::size_t size,
  const std::vector< std::tuple< seed::Seed, Signal, CoverageGain > > &acc_res,
  BigInt sol
) {
  if( solution_cache.find( sol ) != solution_cache.end() ) {
    return acc_res;
  }
  else {
    const auto try_bytes = BigIntToBytes( endian, size, sol );
    const auto try_seed = seed.FixCurBytes( dir, try_bytes );
      // Use dummy value as 'tryVal', since our interest is branch distance.
    const auto [exit_sig,cov_gain,br_info] = executor::GetBranchInfo( sink, opt, try_seed, 0, targ_pt );
    if( br_info && br_info->distance == 0 ) {
      solution_cache.insert( sol );
      std::vector< std::tuple< seed::Seed, Signal, CoverageGain > > new_acc_res;
      new_acc_res.reserve( acc_res.size() + 1u );
      new_acc_res.push_back( std::make_tuple( try_seed, exit_sig, cov_gain ) );
      new_acc_res.insert(
        new_acc_res.end(),
        acc_res.begin(),
        acc_res.end()
      );
      return new_acc_res;
    }
    else {
      return acc_res; // Non-zero branch distance, failed.
      // Target point disappeared, failed.
    }
  }
}

std::vector< std::tuple< seed::Seed, Signal, CoverageGain > >
SolveAsChunk(
  const std::function<void(std::string &&)> &sink,
  const seed::Seed &seed,
  const options::FuzzOption &opt,
  Direction dir,
  const BranchPoint &targ_pt,
  const LinearEquation &lin_eq,
  const std::vector< std::tuple< seed::Seed, Signal, CoverageGain > > &acc_res
) {
  const auto &sols = lin_eq.solutions;
  const auto &size = lin_eq.chunk_size;
  const auto endian = lin_eq.endian;
  auto new_acc_res = acc_res;
  for( const auto &v: sols ) {
    new_acc_res = TryChunkSol( sink, seed, opt, dir, targ_pt, endian, size, new_acc_res, v );
  }  
  return new_acc_res;
}

std::vector< std::tuple< seed::Seed, Signal, CoverageGain > >
SolveEquation(
  const std::function<void(std::string &&)> &sink,
  const seed::Seed &seed,
  const options::FuzzOption &opt,
  Direction dir,
  const std::vector< std::tuple< seed::Seed, Signal, CoverageGain > > &acc_res,
  const BranchPoint &targ_pt,
  const LinearEquation &lin_eq
) {
  if( lin_eq.chunk_size == 1u ) {
    return SolveAsString( sink, seed, opt, targ_pt, lin_eq, acc_res );
  }
  else {
    return SolveAsChunk( sink, seed, opt, dir, targ_pt, lin_eq, acc_res );
  }
}

BigInt GetFunctionValue(
  const Monotonicity &monotonic,
  const BranchInfo &br_info
) {
  const auto sign = ( br_info.branch_type == CompareType::UnsignedSize ) ?
    Signedness::Unsigned :
    Signedness::Signed;
  const auto size = br_info.operand_size;
  if( branch_info::InterpretAs( sign, size, br_info.operand1 ) == monotonic.target_y ) {
    return branch_info::InterpretAs( sign, size, br_info.operand2 );
  }
  else {
    return branch_info::InterpretAs( sign, size, br_info.operand1 );
  }
}

std::vector< std::tuple< seed::Seed, Signal, CoverageGain > >
BinarySearch(
  const std::function<void(std::string &&)> &sink,
  const seed::Seed &seed,
  const options::FuzzOption &opt,
  Direction dir,
  std::size_t max_len,
  const BranchPoint &targ_pt,
  const std::vector< std::tuple< seed::Seed, Signal, CoverageGain > > &acc_res,
  const Monotonicity &mono
) {
  const auto try_val = ( mono.lower_x + mono.upper_x ) / 2;
  const auto endian = ( dir == Direction::Left ) ? Endian::LE : Endian::BE;
  const auto try_bytes = BigIntToBytes( endian, mono.byte_len, try_val );
  const auto try_seed = seed.FixCurBytes( dir, try_bytes );
  const auto [exit_sig,cov_gain,br_info] = executor::GetBranchInfo( sink, opt, try_seed, try_val, targ_pt );
  if( br_info && br_info->distance == 0 ) {
    std::vector< std::tuple< seed::Seed, Signal, CoverageGain > > new_acc_res;
    new_acc_res.reserve( acc_res.size() + 1u );
    new_acc_res.push_back( std::make_tuple( try_seed, exit_sig, cov_gain ) );
    new_acc_res.insert(
      new_acc_res.end(),
      acc_res.begin(),
      acc_res.end()
    );
    return new_acc_res;
  }
  else if( br_info ) {
    const auto new_y = GetFunctionValue( mono, *br_info );
      // TODO : check monotonicity violation, too.p
    const auto new_mono = monotonicity::Update( mono, try_val, new_y );
    if( new_mono.byte_len <= int( max_len ) ) {
      return BinarySearch( sink, seed, opt, dir, max_len, targ_pt, acc_res, new_mono );
    }
    else {
      return acc_res;
    }
  }
  else {
    return acc_res; // Target point disappeared, halt.
  }
}

std::vector< std::tuple< seed::Seed, Signal, CoverageGain > >
SolveMonotonic(
  const std::function<void(std::string &&)> &sink,
  const seed::Seed &seed,
  const options::FuzzOption &opt,
  const std::vector< std::tuple< seed::Seed, Signal, CoverageGain > > &acc_res,
  const BranchPoint &targ_pt,
  const Monotonicity &mono
) {
  const auto max_len_r = seed.QueryUpdateBound( Direction::Right );
  const auto max_len_l = seed.QueryUpdateBound( Direction::Left );
  // Try big endian first, and stop if any result is found.
  auto res = BinarySearch( sink, seed, opt, Direction::Right, max_len_r, targ_pt, {}, mono );
  if( !res.empty() ) {
    res.insert(
      res.end(),
      acc_res.begin(),
      acc_res.end()
    );
    return res;
  }
  else {
    return BinarySearch( sink, seed, opt, Direction::Left, max_len_l, targ_pt, acc_res, mono );
  }
}

bool DifferentSign( const BigInt &i1, const BigInt &i2 ) {
  return ( i1 < 0 && i2 > 0 ) || ( i1 > 0 && i2 < 0 );
}

bool SameSign( const BigInt &i1, const BigInt &i2 ) {
  return ( i1 < 0 && i2 < 0 ) || ( i1 > 0 && i2 > 0 );
}
std::tuple< BigInt, BigInt, Sign >
SplitWithSolution( const BigInt &sol, Sign sign ) {
  return std::make_tuple( BigInt( sol - 1 ), BigInt( sol + 1 ), sign );
}

void GenerateRangesAux(
  BigInt prev_high,
  Sign prev_sign,
  const BigInt &max,
  const std::vector< std::tuple< BigInt, BigInt, Sign > >::const_iterator &split_points_begin,
  const std::vector< std::tuple< BigInt, BigInt, Sign > >::const_iterator &split_points_end,
  std::vector< std::pair< BigInt, BigInt > > &acc_res1,
  std::vector< std::pair< BigInt, BigInt > > &acc_res2
) {
  if( split_points_begin == split_points_end ) {
    if( prev_sign == Sign::Positive ) {
      acc_res2.insert( acc_res2.begin(), std::make_pair( prev_high, max ) );
    }
    else {
      acc_res1.insert( acc_res1.begin(), std::make_pair( prev_high, max ) );
    }
    return;
  }
  const auto & [low,high,sign] = *split_points_begin;
  if( sign == Sign::Positive ) {
    acc_res1.insert( acc_res1.begin(), std::make_pair( prev_high, low ) );
  }
  else {
    acc_res2.insert( acc_res2.begin(), std::make_pair( prev_high, low ) );
  }
  if( high > max ) {
    return;
  }
  else {
    return GenerateRangesAux(
      high,
      sign,
      max,
      std::next( split_points_begin ),
      split_points_end,
      acc_res1,
      acc_res2
    );
  }
}


std::tuple<
  BigInt,
  BigInt,
  Sign
> ExtractMSB(
  unsigned int size,
  const std::tuple<
    BigInt,
    BigInt,
    Sign
  > &v
) {
  const auto &[i1,i2,sign] = v;
  return std::make_tuple(
    BigInt( i1 >> ( ( size - 1u ) * 8u ) ),
    BigInt( i2 >> ( ( size - 1u ) * 8u ) ),
    sign
  );
}

std::pair<
  std::vector< std::pair< BigInt, BigInt > >,
  std::vector< std::pair< BigInt, BigInt > >
>
GenerateRanges(
  const std::vector< std::tuple< BigInt, BigInt, Sign > >::iterator &split_points_begin,
  const std::vector< std::tuple< BigInt, BigInt, Sign > >::iterator &split_points_end,
  unsigned int size
) {
  if( split_points_begin == split_points_end ) {
    return std::pair<
      std::vector< std::pair< BigInt, BigInt > >,
      std::vector< std::pair< BigInt, BigInt > >
    >();
  }
  std::sort(
    split_points_begin,
    split_points_end,
    []( const auto &l, const auto &r ) {
      return std::get< 0 >( l ) < std::get< 0 >( r );
    }
  );
  const auto max = GetUnsignedMax( size );
  std::vector< std::pair< BigInt, BigInt > > acc_res1;
  std::vector< std::pair< BigInt, BigInt > > acc_res2;
  // 'Positive' is used as a dummy argument.
  GenerateRangesAux(
    BigInt( 0 ),
    Sign::Positive,
    max,
    split_points_begin,
    split_points_end,
    acc_res1,
    acc_res2
  );
  return std::make_pair( std::move( acc_res1 ), std::move( acc_res2 ) );
}

  // Currently we consider constraints just for MSB.
std::pair<
  std::vector< std::pair< BigInt, BigInt > >,
  std::vector< std::pair< BigInt, BigInt > >
>
GenerateMSBRanges(
  const std::vector< std::tuple< BigInt, BigInt, Sign > >::iterator &split_points_begin,
  const std::vector< std::tuple< BigInt, BigInt, Sign > >::iterator &split_points_end,
  unsigned int size,
  Signedness sign
) {
  if( split_points_begin == split_points_end ) {
    return std::pair<
      std::vector< std::pair< BigInt, BigInt > >,
      std::vector< std::pair< BigInt, BigInt > >
    >();
  }
  std::sort(
    split_points_begin,
    split_points_end,
    []( const auto &l, const auto &r ) {
      return std::get< 0 >( l ) < std::get< 0 >( r );
    }
  );
  std::for_each(
    split_points_begin,
    split_points_end,
    [size]( auto &v ) {
      v = ExtractMSB( size, v );
    }
  );
  const auto max = ( sign == Signedness::Signed ) ? 127 : 255;
  std::vector< std::pair< BigInt, BigInt > > acc_res1;
  std::vector< std::pair< BigInt, BigInt > > acc_res2;
  // 'Positive' is used as a dummy argument.
  GenerateRangesAux(
    BigInt( 0 ),
    Sign::Positive,
    max,
    split_points_begin,
    split_points_end,
    acc_res1,
    acc_res2
  );
  return std::make_pair( std::move( acc_res1 ), std::move( acc_res2 ) );
}

std::vector< std::pair< BigInt, Sign > > &
CheckSolutionAux(
  const std::function<void(std::string &&)> &sink,
  const seed::Seed &seed,
  const options::FuzzOption &opt,
  Direction dir,
  Endian endian,
  unsigned int size,
  const BranchPoint &targ_pt,
  std::vector< std::pair< BigInt, Sign > > &acc_res,
  const BigInt &sol
) {
  const auto try_bytes = BigIntToBytes( endian, size, sol );
  const auto try_seed = seed.FixCurBytes( dir, try_bytes );
  const auto br_info = executor::GetBranchInfoOnly( sink, opt, try_seed, 0, targ_pt );
  if( br_info && br_info->distance == 0 ) {
    const auto new_try_bytes = BigIntToBytes( endian, size, sol - 1 );
    const auto new_try_seed = seed.FixCurBytes( dir, new_try_bytes );
    const auto new_br_info = executor::GetBranchInfoOnly( sink, opt, new_try_seed, 0, targ_pt );
    if( new_br_info ) {
      const auto sign = ( new_br_info->distance > 0 ) ? Sign::Positive : Sign::Negative;
      acc_res.insert( acc_res.begin(), std::make_pair( sol, sign ) );
      return acc_res;
    }
    else {
      return acc_res;
    }
  }
  else {
    return acc_res;
  }
}

std::vector< std::pair< BigInt, Sign > >
CheckSolution(
  const std::function<void(std::string &&)> &sink,
  const seed::Seed &seed,
  const options::FuzzOption &opt,
  Direction dir,
  const LinearEquation &equation,
  const BranchPoint &targ_pt
) {
  const auto &solutions = equation.solutions;
  const auto endian = equation.endian;
  const auto size = equation.chunk_size;
  const auto sign = equation.linearity.slope.numerator();
  std::vector< std::pair< BigInt, Sign > > acc_res;
  for( const auto &s: solutions ) {
    CheckSolutionAux( sink, seed, opt, dir, endian, size, targ_pt, acc_res, s );
  }
  return acc_res;
}

std::vector< std::tuple< BigInt, BigInt, Sign > > &
CheckSplitAux(
  const std::function<void(std::string &&)> &sink,
  const seed::Seed &seed,
  const options::FuzzOption &opt,
  Direction dir,
  Endian endian,
  unsigned int size,
  const BranchPoint &targ_pt,
  std::vector< std::tuple< BigInt, BigInt, Sign > > &acc_res,
  const BigInt &sol1,
  const BigInt &sol2
) {
  const auto try_bytes1 = BigIntToBytes( endian, size, sol1 );
  const auto try_seed1 = seed.FixCurBytes( dir, try_bytes1 );
  const auto try_bytes2 = BigIntToBytes( endian, size, sol2 );
  const auto try_seed2 = seed.FixCurBytes( dir, try_bytes2 );
    // Use dummy value as 'tryVal', since our interest is in branch distance.
  const auto br_info_opt1 = executor::GetBranchInfoOnly( sink, opt, try_seed1, 0, targ_pt );
  const auto br_info_opt2 = executor::GetBranchInfoOnly( sink, opt, try_seed2, 0, targ_pt );
  if( br_info_opt1 && br_info_opt2 ) {
    if( SameSign( br_info_opt1->distance, br_info_opt2->distance ) ) {
      return acc_res;
    }
    else {
      const auto sign = ( br_info_opt1->distance > 0 ) ? Sign::Positive : Sign::Negative;
      acc_res.insert( acc_res.begin(), std::make_tuple( sol1, sol2, sign ) );
      return acc_res;
    }
  }
  else return acc_res;
}

std::vector< std::tuple< BigInt, BigInt, Sign > >
CheckSplitPoint(
  const std::function<void(std::string &&)> &sink,
  const seed::Seed &seed,
  const options::FuzzOption &opt,
  Direction dir,
  const SimpleLinearInequality &ineq,
  const BranchPoint &targ_pt
) {
  const auto &split_points = ineq.split_points;
  const auto endian = ineq.endian;
  const auto size = ineq.chunk_size;
  std::vector< std::tuple< BigInt, BigInt, Sign > > acc_res;
  for( const auto &s: split_points ) {
    CheckSplitAux( sink, seed, opt, dir, endian, size, targ_pt, acc_res, s.first, s.second );
  }
  return acc_res;
}

std::tuple<
  unsigned int,
  Endian,
  std::vector< std::tuple< BigInt, BigInt, Sign > >
>
ExtractSplitPoint(
  const std::function<void(std::string &&)> &sink,
  const seed::Seed &seed,
  const options::FuzzOption &opt,
  Direction dir,
  const LinearInequality &inequality,
  const BranchPoint &targ_pt
) {
  if( inequality.tight_inequality && inequality.loose_inequality ) {
    const auto tight_sols = CheckSolution( sink, seed, opt, dir, *inequality.tight_inequality, targ_pt );
    if( !tight_sols.empty() ) {
      std::vector< std::tuple< BigInt, BigInt, Sign > > splits;
      std::transform(
        tight_sols.begin(),
        tight_sols.end(),
        std::back_inserter( splits ),
        []( const auto &v ) {
          return SplitWithSolution( v.first, v.second );
        }
      );
      return std::make_tuple(
        inequality.tight_inequality->chunk_size,
        inequality.tight_inequality->endian,
        std::move( splits )
      );
    }
    else {
      const auto splits = CheckSplitPoint( sink, seed, opt, dir, *inequality.loose_inequality, targ_pt );
      return std::make_tuple(
        inequality.loose_inequality->chunk_size,
        inequality.loose_inequality->endian,
        std::move( splits )
      );
    }
  }
  else if( inequality.tight_inequality ) {
    const auto tight_sols = CheckSolution( sink, seed, opt, dir, *inequality.tight_inequality, targ_pt );
    std::vector< std::tuple< BigInt, BigInt, Sign > > splits;
    std::transform(
      tight_sols.begin(),
      tight_sols.end(),
      std::back_inserter( splits ),
      []( const auto &v ) {
        return SplitWithSolution( v.first, v.second );
      }
    );
    return std::make_tuple(
      inequality.tight_inequality->chunk_size,
      inequality.tight_inequality->endian,
      std::move( splits )
    );
  }
  else if( inequality.loose_inequality ) {
    const auto splits = CheckSplitPoint( sink, seed, opt, dir, *inequality.loose_inequality, targ_pt );
    return std::make_tuple(
      inequality.loose_inequality->chunk_size,
      inequality.loose_inequality->endian,
      std::move( splits )
    );
  }
  else {
    failwith( "Unreachable" );
    return std::make_tuple(
      0u,
      Endian::BE,
      std::vector< std::tuple< BigInt, BigInt, Sign > >()
    ); // unreachable
  }
}

std::pair< Constraint, Constraint > 
ExtractCond(
  const std::function<void(std::string &&)> &sink,
  const seed::Seed &seed,
  const options::FuzzOption &opt,
  Direction dir,
  const LinearInequality &ineq,
  const BranchPoint &targ_pt
) {
  auto [size,endian,split_points] = ExtractSplitPoint( sink, seed, opt, dir, ineq, targ_pt );
  const auto sign = ineq.sign;
  const auto &[pos_msb_ranges,neg_msb_ranges] = GenerateMSBRanges(
    split_points.begin(),
    split_points.end(),
    size,
    sign
  );
  auto pos_condition = constraint::Make( pos_msb_ranges, endian, size );
  auto neg_condition = constraint::Make( pos_msb_ranges, endian, size );
  return std::make_pair( pos_condition, neg_condition );
}

std::pair< Constraint, Constraint >
UpdateConditions(
  const Constraint &pc,
  Sign dist_sign,
  const Constraint &cond_p,
  const Constraint &cond_n
) {
  if( dist_sign == Sign::Positive ) {
    return std::make_pair(
      constraint::Conjunction( pc, cond_p ),
      constraint::Conjunction( pc, cond_n )
    );
  }
  else {
    return std::make_pair(
      constraint::Conjunction( pc, cond_n ),
      constraint::Conjunction( pc, cond_p )
    );
  }
}

std::vector< seed::Seed >
EncodeCondition(
  const seed::Seed &seed,
  const options::FuzzOption &/*opt*/,
  Direction dir,
  const Constraint &condition
) {
  if( constraint::IsTop( condition ) ) {
    return std::vector< seed::Seed >();
  }
  std::vector< std::pair< std::size_t, ByteConstraint > > byte_conds;
  byte_conds.reserve( condition.size() );
  if( dir == Direction::Right ) {
    for( std::size_t i = 0u; i != condition.size(); ++i ) {
      const auto &byte_cond = condition[ i ];
      byte_conds.push_back(
        std::make_pair( i, byte_cond )
      );
    }
  }
  else {
    for( std::size_t i = 0u; i != condition.size(); ++i ) {
      const auto &byte_cond = condition[ i ];
      byte_conds.push_back(
        std::make_pair( condition.size() - i - 1, byte_cond )
      );
    }
  }
  std::vector< seed::Seed > acc_seeds;
  acc_seeds.push_back( seed );
  for( const auto &v: byte_conds ) {
    const auto &[offset,byte_cond] = v;
    if( byte_constraint::IsTop( byte_cond ) ) {
    }
    else {
      std::vector< seed::Seed > result;
      for( const auto &range: byte_cond ) {
        auto seeds = std::visit(
          [&]( const auto &v ) -> std::vector< seed::Seed > {
            if constexpr ( std::is_same_v< utils::type_traits::RemoveCvrT< decltype( v ) >, Between > ) {
              const auto low = ( v.low < 0 ) ? std::byte( 0 ) : ( ( v.low > 255 ) ? std::byte( 255 ) : std::byte( std::uint8_t( v.low ) ) );
              const auto high = ( v.high < 0 ) ? std::byte( 0 ) : ( ( v.high > 255 ) ? std::byte( 255 ) : std::byte( std::uint8_t( v.high ) ) );
              const auto mapper = [&]( const seed::Seed &s ) -> seed::Seed {
                return s.ConstrainByteAt( dir, offset, low, high );
              };
              std::vector< seed::Seed > transformed;
              std::transform(
                acc_seeds.begin(),
                acc_seeds.end(),
                std::back_inserter( transformed ),
                mapper
              );
              return transformed;
            }
            else if constexpr ( std::is_same_v< utils::type_traits::RemoveCvrT< decltype( v ) >, Bottom > ) {
              return std::vector< seed::Seed >();
            }
            else if constexpr ( std::is_same_v< utils::type_traits::RemoveCvrT< decltype( v ) >, Top > ) {
              failwith( "Unreachable" );
              return std::vector< seed::Seed >(); // unreachable
            }
          },
          range
        );
        result.insert( result.end(), seeds.begin(), seeds.end() );
      }
      acc_seeds.insert( acc_seeds.end(), result.begin(), result.end() );
    }
  }
  return acc_seeds;
}

std::pair< Constraint, std::vector< seed::Seed > >
SolveInequality(
  const std::function<void(std::string &&)> &sink,
  const seed::Seed &seed,
  const options::FuzzOption &opt,
  Direction dir,
  const Constraint &pc,
  Sign dist_sign,
  const BranchPoint &branch_point,
  const LinearInequality &ineq
) {
  const auto &[cond_p,cond_n] = ExtractCond( sink, seed, opt, dir, ineq, branch_point );
  const auto &[acc_pc,flip_cond] = UpdateConditions( pc, dist_sign, cond_p, cond_n );
  const auto &seeds = EncodeCondition( seed, opt, dir, flip_cond );
  return std::make_pair( acc_pc, seeds );
}

std::pair<
  Constraint,
  std::vector< std::tuple< seed::Seed, Signal, CoverageGain > >
>
SolveBranchCond(
  const std::function<void(std::string &&)> &sink,
  const seed::Seed &seed,
  const options::FuzzOption &opt,
  Direction dir,
  const Constraint &pc,
  const std::pair< BranchCondition, DistanceSign > &branch
) {
  const auto &[branch_cond,dist_sign] = branch;
  const auto &[cond,branch_point] = branch_cond;
  return std::visit(
    [&]( const auto &v ) ->
      std::pair<
        Constraint,
        std::vector< std::tuple< seed::Seed, Signal, CoverageGain > >
      > {
      if constexpr ( std::is_same_v< utils::type_traits::RemoveCvrT< decltype( v ) >, LinEq > ) {
        std::vector< std::tuple< seed::Seed, Signal, CoverageGain > > acc_res;
        auto items = SolveEquation( sink, seed, opt, dir, acc_res, branch_point, v );
        return std::make_pair( pc, std::move( items ) );
      }
      else if constexpr ( std::is_same_v< utils::type_traits::RemoveCvrT< decltype( v ) >, Mono > ) {
        std::vector< std::tuple< seed::Seed, Signal, CoverageGain > > acc_res;
        auto items = SolveMonotonic( sink, seed, opt, acc_res, branch_point, v );
        return std::make_pair( pc, std::move( items ) );
      }
      else if constexpr ( std::is_same_v< utils::type_traits::RemoveCvrT< decltype( v ) >, LinIneq > ) {
        const auto [new_pc,seeds] = SolveInequality( sink, seed, opt, dir, pc, dist_sign, branch_point, v );
        std::vector< std::tuple< seed::Seed, Signal, CoverageGain > > items;
        items.reserve( seeds.size() );
        std::transform(
          seeds.begin(),
          seeds.end(),
          std::back_inserter( items ),
          [&]( const auto &s ) {
            const auto [sig,cov] = executor::GetCoverage( sink, opt, s );
            return std::make_tuple(
              s,
              sig,
              cov
            );
          }
        );
        return std::make_pair( new_pc, std::move( items ) );
      }
    },
    cond
  );
}

std::pair< Constraint, std::vector< std::tuple< seed::Seed, Signal, CoverageGain > > >
SolveBranchSeq(
  const std::function<void(std::string &&)> &sink,
  const seed::Seed &seed_,
  const options::FuzzOption &opt,
  Direction dir,
  const Constraint &pc_,
  const BranchSeq &branch_seq
) {
  Constraint pc = pc_;
  std::vector< std::tuple< seed::Seed, Signal, CoverageGain > > acc_seeds;
  for( const auto &branch: branch_seq.branches ) {
    auto [acc_pc,new_seeds] = SolveBranchCond( sink, seed_, opt, dir, pc, branch );
    pc = std::move( acc_pc );
    acc_seeds.insert( acc_seeds.begin(), new_seeds.begin(), new_seeds.end() );
  }
  return std::make_pair( std::move( pc ), std::move( acc_seeds ) );
}

std::vector< std::tuple< seed::Seed, Signal, CoverageGain > >
SolveBranchTree(
  const std::function<void(std::string &&)> &sink,
  const seed::Seed &seed_,
  const options::FuzzOption &opt,
  Direction dir,
  const Constraint &pc,
  const BranchTree &branch_tree
) {
  return boost::apply_visitor(
    [&]( const auto &v ) -> std::vector< std::tuple< seed::Seed, Signal, CoverageGain > > {
      if constexpr ( std::is_same_v< utils::type_traits::RemoveCvrT< decltype( v ) >, Straight > ) {
        auto [new_pc,new_seeds] = SolveBranchSeq( sink, seed_, opt, dir, pc, v );
        const auto terminal_seeds = EncodeCondition( seed_, opt, dir, new_pc );
        std::vector< std::tuple< seed::Seed, Signal, CoverageGain > > terminal_items;
        terminal_items.reserve( terminal_seeds.size() );
        std::transform(
          terminal_seeds.begin(),
          terminal_seeds.end(),
          std::back_inserter( terminal_items ),
          [&]( const auto &s ) -> std::tuple< seed::Seed, Signal, CoverageGain > {
            const auto [sig,cov] = executor::GetCoverage( sink, opt, s );
            return std::make_tuple(
              s,
              sig,
              cov
            );
          }
        );
        terminal_items.insert( terminal_items.begin(), new_seeds.begin(), new_seeds.end() );
        return terminal_items;
      }
      else if constexpr ( std::is_same_v< utils::type_traits::RemoveCvrT< decltype( v ) >, ForkedTree > ) {
        const auto &[branch_seq,condition,childs] = v;
        const auto [new_pc,new_seeds] = SolveBranchSeq( sink, seed_, opt, dir, pc, branch_seq );
        return std::visit(
          [&]( const auto &v ) -> std::vector< std::tuple< seed::Seed, Signal, CoverageGain > > {
            if constexpr ( std::is_same_v< utils::type_traits::RemoveCvrT< decltype( v ) >, LinIneq > ) {
              const auto &ineq = v;
              const auto &branch_pt = condition.second;
              const auto [cond_p,cond_n] = ExtractCond( sink, seed_, opt, dir, ineq, branch_pt );
              std::vector< std::tuple< seed::Seed, Signal, CoverageGain > > child_seeds;
              for( const auto &c: childs ) {
                const auto &[dist_sign,child_tree] = c;
                const auto [new_new_pc,unused] = UpdateConditions( new_pc, dist_sign, cond_p, cond_n );
                auto s = SolveBranchTree( sink, seed_, opt, dir, new_new_pc, child_tree );
                child_seeds.insert( child_seeds.end(), s.begin(), s.end() );
              }
              child_seeds.insert( child_seeds.begin(), new_seeds.begin(), new_seeds.end() );
              return child_seeds;
            }
            else {
              std::vector< std::tuple< seed::Seed, Signal, CoverageGain > > child_seeds;
              for( const auto &c: childs ) {
                const auto &[dist_sign,child_tree] = c;
                auto s = SolveBranchTree( sink, seed_, opt, dir, new_pc, child_tree );
                child_seeds.insert( child_seeds.end(), s.begin(), s.end() );
              }
              child_seeds.insert( child_seeds.begin(), new_seeds.begin(), new_seeds.end() );
              return child_seeds;
            }
          },
          condition.first
        );
      }
      else if constexpr ( std::is_same_v< utils::type_traits::RemoveCvrT< decltype( v ) >, DivergeTree > ) {
        const auto &[branch_seq,sub_trees] = v;
        const auto [new_pc,new_seeds] = SolveBranchSeq( sink, seed_, opt, dir, pc, branch_seq );
        std::vector< std::tuple< seed::Seed, Signal, CoverageGain > > sub_tree_seeds;
        for( const auto &c: sub_trees ) {
          auto s = SolveBranchTree( sink, seed_, opt, dir, new_pc, c );
          sub_tree_seeds.insert( sub_tree_seeds.end(), s.begin(), s.end() );
        }
        sub_tree_seeds.insert( sub_tree_seeds.begin(), new_seeds.begin(), new_seeds.end() );
        return sub_tree_seeds;
      }
    },
    branch_tree
  );
}


/*
  let rec generateRangesAux prevHigh prevSign max splitPoints accRes1 accRes2 =
    match splitPoints with
    | [] ->
      let accRes1, accRes2 =
        if prevSign = Positive
        then accRes1, (prevHigh, max) :: accRes2
        else (prevHigh, max) :: accRes1, accRes2
      accRes1, accRes2
    | (low, high, sign) :: tailSplitPoints ->
      let accRes1, accRes2 =
        if sign = Positive
        then (prevHigh, low) :: accRes1, accRes2
        else accRes1, (prevHigh, low) :: accRes2
      if high > max then (accRes1, accRes2) else
        generateRangesAux high sign max tailSplitPoints accRes1 accRes2

  let extractMSB size (i1:bigint, i2:bigint, sign) =
    (i1 >>> ((size - 1) * 8), i2 >>> ((size - 1) * 8), sign)

  // Currently we consider constraints just for MSB.
  let rec generateMSBRanges splitPoints size sign =
    if List.isEmpty splitPoints then [], [] else
      let splitPoints = List.sortBy (fun (x, _, _) -> x) splitPoints
      let splitPoints = List.map (extractMSB size) splitPoints
      let max = if sign = Signed then 127I else 255I
      // 'Positive' is used as a dummy argument.
      generateRangesAux 0I Positive max splitPoints [] []

  let rec generateRanges splitPoints size =
    if List.isEmpty splitPoints then [], [] else
      let splitPoints = List.sortBy (fun (x, _, _) -> x) splitPoints
      let max = getUnsignedMax size
      // 'Positive' is used as a dummy argument.
      generateRangesAux 0I Positive max splitPoints [] []

  let extractMSB size (i1:bigint, i2:bigint, sign) =
    (i1 >>> ((size - 1) * 8), i2 >>> ((size - 1) * 8), sign)

  // Currently we consider constraints just for MSB.
  let rec generateMSBRanges splitPoints size sign =
    if List.isEmpty splitPoints then [], [] else
      let splitPoints = List.sortBy (fun (x, _, _) -> x) splitPoints
      let splitPoints = List.map (extractMSB size) splitPoints
      let max = if sign = Signed then 127I else 255I
      // 'Positive' is used as a dummy argument.
      generateRangesAux 0I Positive max splitPoints [] []

  let rec generateRanges splitPoints size =
    if List.isEmpty splitPoints then [], [] else
      let splitPoints = List.sortBy (fun (x, _, _) -> x) splitPoints
      let max = getUnsignedMax size
      // 'Positive' is used as a dummy argument.
      generateRangesAux 0I Positive max splitPoints [] []

  let checkSolutionAux seed opt dir endian size targPt accRes sol =
    let tryBytes = bigIntToBytes endian size sol
    let trySeed = Seed.fixCurBytes seed dir tryBytes
    // Use dummy value as 'tryVal', since our interest is in branch distance.
    match Executor.getBranchInfoOnly opt trySeed 0I targPt with
    | Some brInfo when brInfo.Distance = 0I ->
      let tryBytes' = bigIntToBytes endian size (sol - 1I)
      let trySeed' = Seed.fixCurBytes seed dir tryBytes'
      match Executor.getBranchInfoOnly opt trySeed' 0I targPt with
      | Some brInfo' ->
        let sign = if brInfo'.Distance > 0I then Positive else Negative
        (sol, sign) :: accRes
      | None -> accRes
    | _ -> accRes

  let checkSolution seed opt dir equation targPt =
    let solutions = equation.Solutions
    let endian = equation.Endian
    let size = equation.ChunkSize
    let sign = equation.Linearity.Slope.Numerator
    List.fold (checkSolutionAux seed opt dir endian size targPt) [] solutions

  let checkSplitAux seed opt dir endian size targPt accRes (sol1, sol2) =
    let tryBytes1 = bigIntToBytes endian size sol1
    let trySeed1 = Seed.fixCurBytes seed dir tryBytes1
    let tryBytes2 = bigIntToBytes endian size sol2
    let trySeed2 = Seed.fixCurBytes seed dir tryBytes2
    // Use dummy value as 'tryVal', since our interest is in branch distance.
    let brInfoOpt1 = Executor.getBranchInfoOnly opt trySeed1 0I targPt
    let brInfoOpt2 = Executor.getBranchInfoOnly opt trySeed2 0I targPt
    match brInfoOpt1, brInfoOpt2 with
    | Some brInfo1, Some brInfo2 ->
      if sameSign brInfo1.Distance brInfo2.Distance
      then accRes
      else let sign = if brInfo1.Distance > 0I then Positive else Negative
           (sol1, sol2, sign) :: accRes
    | _ -> accRes

  let checkSplitPoint seed opt dir ineq targPt =
    let splitPoints = ineq.SplitPoints
    let endian = ineq.Endian
    let size = ineq.ChunkSize
    List.fold (checkSplitAux seed opt dir endian size targPt) [] splitPoints

  let extractSplitPoint seed opt dir inequality targPt =
    match inequality.TightInequality, inequality.LooseInequality with
    | Some eq, Some ineq ->
      let tightSols = checkSolution seed opt dir eq targPt
      if not (List.isEmpty tightSols) then
        let splits = List.map splitWithSolution tightSols
        (eq.ChunkSize, eq.Endian, splits)
      else
        let splits = checkSplitPoint seed opt dir ineq targPt
        (ineq.ChunkSize, ineq.Endian, splits)
    | Some eq, None ->
      let tightSols = checkSolution seed opt dir eq targPt
      let splits = List.map splitWithSolution tightSols
      (eq.ChunkSize, eq.Endian, splits)
    | None, Some ineq ->
      let splits = checkSplitPoint seed opt dir ineq targPt
      (ineq.ChunkSize, ineq.Endian, splits)
    | None, None -> failwith "Unreachable"

  let extractCond seed opt dir ineq targPt =
    let size, endian, splitPoints = extractSplitPoint seed opt dir ineq targPt
    let sign = ineq.Sign
    let posMSBRanges, negMSBRanges = generateMSBRanges splitPoints size sign
    let posCondition = Constraint.make posMSBRanges endian size
    let negCondition = Constraint.make negMSBRanges endian size
    (posCondition, negCondition)

  let updateConditions pc distSign (condP:Constraint) (condN:Constraint) =
    if distSign = Positive
    then (Constraint.conjunction pc condP, Constraint.conjunction pc condN)
    else (Constraint.conjunction pc condN, Constraint.conjunction pc condP)


  let encodeCondition seed opt dir condition =
    if Constraint.isTop condition then []
    elif Seed.queryLenToward seed dir < List.length condition then []
    else
      let byteConds =
        if dir = Right
        then List.mapi (fun i byteCond -> (i, byteCond)) condition
        else let len = List.length condition
             List.mapi (fun i byteCond -> (len - i - 1, byteCond)) condition
      let newSeeds =
        List.fold (fun accSeeds (offset, byteCond) ->
          if ByteConstraint.isTop byteCond then accSeeds else
            List.collect (fun range ->
              match range with
              | Between (low, high) ->
                let low = if low < 0I then 0uy
                          elif low > 255I then 255uy
                          else byte low
                let high = if high < 0I then 0uy
                           elif high > 255I then 255uy
                           else byte high
                let mapper s = Seed.constrainByteAt s dir offset low high
                List.map mapper accSeeds
              | Bottom -> []
              | Top -> failwith "Unreachable"
            ) byteCond
          ) [seed] byteConds
      newSeeds

  let solveInequality seed opt dir pc distSign branchPoint ineq =
    let condP, condN = extractCond seed opt dir ineq branchPoint
    let accPc, flipCond = updateConditions pc distSign condP condN
    let seeds = encodeCondition seed opt dir flipCond
    (accPc, seeds)

  let solveBranchCond seed opt dir (pc: Constraint) branch =
    let branchCond, distSign = branch
    let cond, branchPoint = branchCond
    match cond with
    | LinEq linEq ->
      let items = solveEquation seed opt dir [] (branchPoint, linEq)
      (pc, items)
    | Mono mono ->
      let items = solveMonotonic seed opt [] (branchPoint, mono)
      (pc, items)
    | LinIneq ineq ->
      let pc, seeds = solveInequality seed opt dir pc distSign branchPoint ineq
      let sigs, covs = List.map (Executor.getCoverage opt) seeds |> List.unzip
      let items = List.zip3 seeds sigs covs
      (pc, items)

  let solveBranchSeq seed opt dir pc branchSeq =
    List.fold (fun (accPc, accSeeds) branch ->
      let accPc, newSeeds = solveBranchCond seed opt dir accPc branch
      accPc, newSeeds @ accSeeds
    ) (pc, []) branchSeq.Branches


  let rec solveBranchTree seed opt dir pc branchTree =
    match branchTree with
    | Straight branchSeq ->
      let pc, newSeeds = solveBranchSeq seed opt dir pc branchSeq
      let terminalSeeds = encodeCondition seed opt dir pc
      let results = List.map (Executor.getCoverage opt) terminalSeeds
      let sigs, covs = List.unzip results
      let terminalItems = List.zip3 terminalSeeds sigs covs
      newSeeds @ terminalItems
    | ForkedTree (branchSeq, (LinIneq ineq, branchPt), childs) ->
      let pc, newSeeds = solveBranchSeq seed opt dir pc branchSeq
      let condP, condN = extractCond seed opt dir ineq branchPt
      let childSeeds =
        List.map (fun (distSign, childTree) ->
          let pc, _ = updateConditions pc distSign condP condN
          solveBranchTree seed opt dir pc childTree
        ) childs
      List.concat (newSeeds :: childSeeds)
    | ForkedTree (branchSeq, _, childs) ->
      let pc, newSeeds = solveBranchSeq seed opt dir pc branchSeq
      let childSeeds = List.map (snd >> solveBranchTree seed opt dir pc) childs
      List.concat (newSeeds :: childSeeds)
    | DivergeTree (branchSeq, subTrees) ->
      let pc, newSeeds = solveBranchSeq seed opt dir pc branchSeq
      let subTreeSeeds = List.map (solveBranchTree seed opt dir pc) subTrees
      List.concat (newSeeds :: subTreeSeeds)
*/

std::vector< std::tuple< seed::Seed, Signal, CoverageGain > >
Solve(
  const std::function<void(std::string &&)> &sink,
  const seed::Seed &seed,
  const options::FuzzOption &opt,
  Direction byte_dir,
  const BranchTree &branch_tree
){
  const auto init_pc = constraint::top;
  return SolveBranchTree( sink, seed, opt, byte_dir, init_pc, branch_tree );
}
  
}

}

