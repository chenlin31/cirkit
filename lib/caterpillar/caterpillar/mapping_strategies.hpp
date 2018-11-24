/*------------------------------------------------------------------------------
| This file is distributed under the MIT License.
| See accompanying file /LICENSE for details.
| Author(s): Mathias Soeken and Giulia Meuli
*-----------------------------------------------------------------------------*/
#include <cstdint>

#include "sat.hpp"

namespace caterpillar
{

namespace mt = mockturtle;

#pragma region has_set_pebble_limit
template<class MappingStrategy, class = void>
struct has_set_pebble_limit : std::false_type
{
};

template<class MappingStrategy>
struct has_set_pebble_limit<MappingStrategy, std::void_t<decltype( std::declval<MappingStrategy>().set_pebble_limit( uint32_t() ) )>> : std::true_type
{
};

template<class MappingStrategy>
inline constexpr bool has_set_pebble_limit_v = has_set_pebble_limit<MappingStrategy>::value;
#pragma endregion

template<class LogicNetwork>
class pebbling_mapping_strategy
{
  
public:
  /* returns the method foreach_step */
  pebbling_mapping_strategy( LogicNetwork const& ntk, uint32_t const pebbles = 0)
  {

    // clang-format off
            static_assert(mt::is_network_type_v<LogicNetwork>, "LogicNetwork is not a network type");
            static_assert(mt::has_is_pi_v<LogicNetwork>, "LogicNetwork does not implement the is_pi method");
            static_assert(mt::has_foreach_fanin_v<LogicNetwork>, "LogicNetwork does not implement the foreach_fanin method");
            static_assert(mt::has_foreach_gate_v<LogicNetwork>, "LogicNetwork does not implement the foreach_gate method");
            static_assert(mt::has_num_gates_v<LogicNetwork>, "LogicNetwork does not implement the num_gates method");
            static_assert(mt::has_foreach_po_v<LogicNetwork>, "LogicNetwork does not implement the foreach_po method");
            static_assert(mt::has_index_to_node_v<LogicNetwork>, "LogicNetwork does not implement the index_to_node method");
    // clang-format on
    

		auto man = pebble_solver_man<LogicNetwork>(ntk, pebbles);
    steps = man.get_steps();

  }

  void set_pebble_limit(uint32_t limit)
  {
    limit_ = limit;
  }



  template<class Fn>
  inline void foreach_step( Fn&& fn ) const
  {
    for ( auto const& [n, a] : steps )
      fn( n, a );
  }
private:
  std::vector<std::pair<mt::node<LogicNetwork>, mapping_strategy_action>> steps;
  uint32_t limit_{50u};
};

template<class LogicNetwork>
class bennett_mapping_strategy
{
public:
  bennett_mapping_strategy( LogicNetwork const& ntk )
  {
    // clang-format off
            static_assert(mt::is_network_type_v<LogicNetwork>, "LogicNetwork is not a network type");
            static_assert(mt::has_foreach_po_v<LogicNetwork>, "LogicNetwork does not implement the foreach_po method");
            static_assert(mt::has_is_constant_v<LogicNetwork>, "LogicNetwork does not implement the is_constant method");
            static_assert(mt::has_is_pi_v<LogicNetwork>, "LogicNetwork does not implement the is_pi method");
            static_assert(mt::has_get_node_v<LogicNetwork>, "LogicNetwork does not implement the get_node method");
    // clang-format on

    std::unordered_set<mt::node<LogicNetwork>> drivers;
    ntk.foreach_po( [&]( auto const& f ) { drivers.insert( ntk.get_node( f ) ); } );

    auto it = steps.begin();
    mt::topo_view view{ntk};
    view.foreach_node( [&]( auto n ) {
      if ( ntk.is_constant( n ) || ntk.is_pi( n ) )
        return true;

      /* compute step */
      it = steps.insert( it, {n, compute_action{}} );
      ++it;

      if ( !drivers.count( n ) )
        it = steps.insert( it, {n, uncompute_action{}} );

      return true;
    } );
  }

  template<class Fn>
  inline void foreach_step( Fn&& fn ) const
  {
    for ( auto const& [n, a] : steps )
      fn( n, a );
  }

private:
  std::vector<std::pair<mt::node<LogicNetwork>, mapping_strategy_action>> steps;
};

template<class LogicNetwork>
class bennett_inplace_mapping_strategy
{
public:
  bennett_inplace_mapping_strategy( LogicNetwork const& ntk )
  {
    // clang-format off
            static_assert(mt::is_network_type_v<LogicNetwork>, "LogicNetwork is not a network type");
            static_assert(mt::has_foreach_po_v<LogicNetwork>, "LogicNetwork does not implement the foreach_po method");
            static_assert(mt::has_is_constant_v<LogicNetwork>, "LogicNetwork does not implement the is_constant method");
            static_assert(mt::has_is_pi_v<LogicNetwork>, "LogicNetwork does not implement the is_pi method");
            static_assert(mt::has_get_node_v<LogicNetwork>, "LogicNetwork does not implement the get_node method");
            static_assert(mt::has_node_to_index_v<LogicNetwork>, "LogicNetwork does not implement the node_to_index method");
            static_assert(mt::has_clear_values_v<LogicNetwork>, "LogicNetwork does not implement the clear_values method");
            static_assert(mt::has_set_value_v<LogicNetwork>, "LogicNetwork does not implement the set_value method");
            static_assert(mt::has_decr_value_v<LogicNetwork>, "LogicNetwork does not implement the decr_value method");
            static_assert(mt::has_fanout_size_v<LogicNetwork>, "LogicNetwork does not implement the fanout_size method");
            static_assert(mt::has_foreach_fanin_v<LogicNetwork>, "LogicNetwork does not implement the foreach_fanin method");
    // clang-format on

    std::unordered_set<mt::node<LogicNetwork>> drivers;
    ntk.foreach_po( [&]( auto const& f ) { drivers.insert( ntk.get_node( f ) ); } );

    ntk.clear_values();
    ntk.foreach_node( [&]( const auto& n ) { ntk.set_value( n, ntk.fanout_size( n ) ); } );

    auto it = steps.begin();
    //mt::topo_view view{ntk};
    ntk.foreach_node( [&]( auto n ) {
      if ( ntk.is_constant( n ) || ntk.is_pi( n ) )
        return true;

      /* decrease reference counts and mark potential target for inplace */
      int target{-1};
      ntk.foreach_fanin( n, [&]( auto f ) {
        if ( ntk.decr_value( ntk.get_node( f ) ) == 0 )
        {
          if ( target == -1 )
          {
            target = ntk.node_to_index( ntk.get_node( f ) );
          }
        }
      } );

      /* check for inplace (only if there is possible target and node is not an output driver) */
      if ( target != -1 && !drivers.count( n ) )
      {
        if constexpr ( mt::has_is_xor_v<LogicNetwork> )
        {
          if ( ntk.is_xor( n ) )
          {
            it = steps.insert( it, {n, compute_inplace_action{
                                           static_cast<uint32_t>(
                                               target )}} );
            ++it;
            it = steps.insert( it, {n, uncompute_inplace_action{
                                           static_cast<uint32_t>(
                                               target )}} );
            return true;
          }
        }
        if constexpr ( mt::has_is_xor3_v<LogicNetwork> )
        {
          if ( ntk.is_xor3( n ) )
          {
            it = steps.insert( it, {n, compute_inplace_action{
                                           static_cast<uint32_t>(
                                               target )}} );
            ++it;
            it = steps.insert( it, {n, uncompute_inplace_action{
                                           static_cast<uint32_t>(
                                               target )}} );
            return true;
          }
        }
      }

      /* compute step */
      it = steps.insert( it, {n, compute_action{}} );
      ++it;

      if ( !drivers.count( n ) )
        it = steps.insert( it, {n, uncompute_action{}} );

      return true;
    } );
  }

  template<class Fn>
  inline void foreach_step( Fn&& fn ) const
  {
    for ( auto const& [n, a] : steps )
      fn( n, a );
  }

private:
  std::vector<std::pair<mt::node<LogicNetwork>, mapping_strategy_action>> steps;
};

} // namespace caterpillar