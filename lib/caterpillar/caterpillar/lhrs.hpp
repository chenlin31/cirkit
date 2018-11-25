/*------------------------------------------------------------------------------
| This file is distributed under the MIT License.
| See accompanying file /LICENSE for details.
| Author(s): Mathias Soeken
| Author(s): Giulia Meuli
*-----------------------------------------------------------------------------*/
#pragma once

#include <array>
#include <cstdint>
#include <fmt/format.h>
#include <mockturtle/traits.hpp>
#include <mockturtle/utils/node_map.hpp>
#include <mockturtle/utils/stopwatch.hpp>
#include <mockturtle/views/topo_view.hpp>
#include <stack>
#include <tweedledum/gates/gate_kinds.hpp>
#include <variant>
#include <vector>

#include "mapping_strategies.hpp"
#include <mockturtle/algorithms/cut_enumeration/spectr_cut.hpp>

namespace caterpillar
{

namespace detail
{
template<class... Ts>
struct overloaded : Ts...
{
  using Ts::operator()...;
};
template<class... Ts>
overloaded( Ts... )->overloaded<Ts...>;
} // namespace detail

namespace mt = mockturtle;

struct logic_network_synthesis_params
{
  /*! \brief Maximum number of pebbles to use, if supported by mapping strategy (0 means no limit) */
  uint32_t pebble_limit{0u};
  bool verbose{false};
};

struct logic_network_synthesis_stats
{
  /*! \brief Total runtime. */
  mockturtle::stopwatch<>::duration time_total{0};

  /*! \brief Required number of ancilla. */
  uint32_t required_ancillae{0u};

  void report() const
  {
    std::cout << fmt::format( "[i] total time = {:>5.2f} secs\n", mockturtle::to_seconds( time_total ) );
  }
};

namespace detail
{

template<class QuantumNetwork, class LogicNetwork, class MappingStrategy, class SingleTargetGateSynthesisFn>
class logic_network_synthesis_impl
{
public:
  logic_network_synthesis_impl( QuantumNetwork& qnet, LogicNetwork const& ntk,
                                SingleTargetGateSynthesisFn const& stg_fn,
                                logic_network_synthesis_params const& ps,
                                logic_network_synthesis_stats& st )
      : qnet( qnet ), ntk( ntk ), stg_fn( stg_fn ), ps( ps ), st( st ), node_to_qubit( ntk )
  {
  }

  void run()
  {
    mockturtle::stopwatch t( st.time_total );
    prepare_inputs();
    prepare_constant( false );
    if ( ntk.get_node( ntk.get_constant( false ) ) != ntk.get_node( ntk.get_constant( true ) ) )
      prepare_constant( true );

    MappingStrategy strategy( ntk );
    if constexpr ( has_set_pebble_limit_v<MappingStrategy> )
    {
      strategy.set_pebble_limit( ps.pebble_limit );
    }
    strategy.foreach_step( [&]( auto node, auto action ) {
      std::visit(
          overloaded{
              []( auto arg ) {},
              [&]( compute_action const& ) {
                const auto t = node_to_qubit[node] = request_ancilla();
                if ( ps.verbose )
                  std::cout << "[i] compute " << ntk.node_to_index( node ) << " in qubit " << t << "\n";
                compute_node( node, t );
              },
              [&]( uncompute_action const& ) {
                const auto t = node_to_qubit[node];
                if ( ps.verbose )
                  std::cout << "[i] uncompute " << ntk.node_to_index( node ) << " from qubit " << t << "\n";
                compute_node( node, t );
                release_ancilla( t );
              },
              [&]( compute_inplace_action const& action ) {
                if ( ps.verbose )
                  std::cout << "[i] compute " << ntk.node_to_index( node ) << " inplace onto " << action.target_index << " in qubit " << node_to_qubit[ntk.index_to_node( action.target_index )] << "\n";
                const auto t = node_to_qubit[node] = node_to_qubit[ntk.index_to_node( action.target_index )];
                compute_node_inplace( node, t );
              },
              [&]( uncompute_inplace_action const& action ) {
                if ( ps.verbose )
                  std::cout << "[i] uncompute " << ntk.node_to_index( node ) << " inplace onto " << action.target_index << " from qubit " << node_to_qubit[ntk.index_to_node( action.target_index )] << "\n";
                const auto t = node_to_qubit[node];
                compute_node_inplace( node, t );
              }},
          action );
    } );
  }

private:
  void prepare_inputs()
  {
    /* prepare primary inputs of logic network */
    ntk.foreach_pi( [&]( auto n ) {
      node_to_qubit[n] = qnet.num_qubits();
      qnet.add_qubit();
    } );
  }

  void prepare_constant( bool value )
  {
    const auto f = ntk.get_constant( value );
    const auto n = ntk.get_node( f );
    if ( ntk.fanout_size( n ) == 0 )
      return;
    const auto v = ntk.constant_value( n ) ^ ntk.is_complemented( f );
    node_to_qubit[n] = qnet.num_qubits();
    qnet.add_qubit();
    if ( v )
      qnet.add_gate( tweedledum::gate_kinds_t::pauli_x, node_to_qubit[n] );
  }

  uint32_t request_ancilla()
  {
    if ( free_ancillae.empty() )
    {
      const auto r = qnet.num_qubits();
      st.required_ancillae++;
      qnet.add_qubit();
      return r;
    }
    else
    {
      const auto r = free_ancillae.top();
      free_ancillae.pop();
      return r;
    }
  }

  void release_ancilla( uint32_t q )
  {
    free_ancillae.push( q );
  }

  template<int Fanin>
  std::array<uint32_t, Fanin> get_fanin_as_literals( mt::node<LogicNetwork> const& n )
  {
    std::array<uint32_t, Fanin> controls;
    ntk.foreach_fanin( n, [&]( auto const& f, auto i ) {
      controls[i] = ( ntk.node_to_index( ntk.get_node( f ) ) << 1 ) | ntk.is_complemented( f );
    } );
    return controls;
  }

  std::vector<uint32_t> get_fanin_as_qubits( mt::node<LogicNetwork> const& n )
  {
    std::vector<uint32_t> controls;
    ntk.foreach_fanin( n, [&]( auto const& f, auto i ) {
      assert( !ntk.is_complemented( f ) );
      controls.push_back( node_to_qubit[ntk.node_to_index( ntk.get_node( f ) )] );
    } );
    return controls;
  }

  void compute_node( mt::node<LogicNetwork> const& node, uint32_t t )
  {
    if constexpr ( mt::has_is_and_v<LogicNetwork> )
    {
      if ( ntk.is_and( node ) )
      {
        auto controls = get_fanin_as_literals<2>( node );
        compute_and( node_to_qubit[ntk.index_to_node( controls[0] >> 1 )],
                     node_to_qubit[ntk.index_to_node( controls[1] >> 1 )],
                     controls[0] & 1, controls[1] & 1, t );
        return;
      }
    }
    if constexpr ( mt::has_is_or_v<LogicNetwork> )
    {
      if ( ntk.is_or( node ) )
      {
        auto controls = get_fanin_as_literals<2>( node );
        compute_or( node_to_qubit[ntk.index_to_node( controls[0] >> 1 )],
                    node_to_qubit[ntk.index_to_node( controls[1] >> 1 )],
                    controls[0] & 1, controls[1] & 1, t );
        return;
      }
    }
    if constexpr ( mt::has_is_xor_v<LogicNetwork> )
    {
      if ( ntk.is_xor( node ) )
      {
        auto controls = get_fanin_as_literals<2>( node );
        compute_xor( node_to_qubit[ntk.index_to_node( controls[0] >> 1 )],
                     node_to_qubit[ntk.index_to_node( controls[1] >> 1 )],
                     ( controls[0] & 1 ) != ( controls[1] & 1 ), t );
        return;
      }
    }
    if constexpr ( mt::has_is_xor3_v<LogicNetwork> )
    {
      if ( ntk.is_xor3( node ) )
      {
        auto controls = get_fanin_as_literals<3>( node );

        /* Is XOR3 in fact an XOR2? */
        if ( ntk.is_constant( ntk.index_to_node( controls[0] >> 1 ) ) )
        {
          compute_xor( node_to_qubit[ntk.index_to_node( controls[1] >> 1 )],
                       node_to_qubit[ntk.index_to_node( controls[2] >> 1 )],
                       ( ( controls[0] & 1 ) != ( controls[1] & 1 ) ) != ( controls[2] & 1 ),
                       t );
        }
        else
        {
          compute_xor3(
              node_to_qubit[ntk.index_to_node( controls[0] >> 1 )],
              node_to_qubit[ntk.index_to_node( controls[1] >> 1 )],
              node_to_qubit[ntk.index_to_node( controls[2] >> 1 )],
              ( ( controls[0] & 1 ) != ( controls[1] & 1 ) ) != ( controls[2] & 1 ),
              t );
        }
        return;
      }
    }
    if constexpr ( mt::has_is_maj_v<LogicNetwork> )
    {
      if ( ntk.is_maj( node ) )
      {
        auto controls = get_fanin_as_literals<3>( node );
        /* Is XOR3 in fact an AND or OR? */
        if ( ntk.is_constant( ntk.index_to_node( controls[0] >> 1 ) ) )
        {
          if ( controls[0] & 1 )
          {
            compute_or(
                node_to_qubit[ntk.index_to_node( controls[1] >> 1 )],
                node_to_qubit[ntk.index_to_node( controls[2] >> 1 )],
                controls[1] & 1, controls[2] & 1, t );
          }
          else
          {
            compute_and(
                node_to_qubit[ntk.index_to_node( controls[1] >> 1 )],
                node_to_qubit[ntk.index_to_node( controls[2] >> 1 )],
                controls[1] & 1, controls[2] & 1, t );
          }
        }
        else
        {
          compute_maj(
              node_to_qubit[ntk.index_to_node( controls[0] >> 1 )],
              node_to_qubit[ntk.index_to_node( controls[1] >> 1 )],
              node_to_qubit[ntk.index_to_node( controls[2] >> 1 )],
              controls[0] & 1, controls[1] & 1, controls[2] & 1, t );
        }
        return;
      }
    }
    if constexpr ( mt::has_node_function_v<LogicNetwork> )
    {
      kitty::dynamic_truth_table tt = ntk.node_function( node );
      auto clone = tt.construct();
      kitty::create_parity( clone );

      if ( tt == clone )
      {
        const auto controls = get_fanin_as_qubits( node );
        compute_xor_block( controls, t );
      }
      else
      {
        // In this case, the procedure works a bit different and retrieves the
        // controls directly as mapped qubits.  We assume that the inputs cannot
        // be complemented, e.g., in the case of k-LUT networks.
        const auto controls = get_fanin_as_qubits( node );
        compute_lut( ntk.node_function( node ), controls, t );
      }
    }
  }

  void compute_node_inplace( mt::node<LogicNetwork> const& node, uint32_t t )
  {
    if constexpr ( mt::has_is_xor_v<LogicNetwork> )
    {
      if ( ntk.is_xor( node ) )
      {
        auto controls = get_fanin_as_literals<2>( node );
        compute_xor_inplace( node_to_qubit[ntk.index_to_node( controls[0] >> 1 )],
                             node_to_qubit[ntk.index_to_node( controls[1] >> 1 )],
                             ( controls[0] & 1 ) != ( controls[1] & 1 ), t );
        return;
      }
    }
    if constexpr ( mt::has_is_xor3_v<LogicNetwork> )
    {
      if ( ntk.is_xor3( node ) )
      {
        auto controls = get_fanin_as_literals<3>( node );

        /* Is XOR3 in fact an XOR2? */
        if ( ntk.is_constant( ntk.index_to_node( controls[0] >> 1 ) ) )
        {
          compute_xor_inplace(
              node_to_qubit[ntk.index_to_node( controls[1] >> 1 )],
              node_to_qubit[ntk.index_to_node( controls[2] >> 1 )],
              ( ( controls[0] & 1 ) != ( controls[1] & 1 ) ) != ( controls[2] & 1 ),
              t );
        }
        else
        {
          compute_xor3_inplace(
              node_to_qubit[ntk.index_to_node( controls[0] >> 1 )],
              node_to_qubit[ntk.index_to_node( controls[1] >> 1 )],
              node_to_qubit[ntk.index_to_node( controls[2] >> 1 )],
              ( ( controls[0] & 1 ) != ( controls[1] & 1 ) ) != ( controls[2] & 1 ),
              t );
        }
        return;
      }
    }
    if constexpr ( mt::has_node_function_v<LogicNetwork> )
    {
      const auto controls = get_fanin_as_qubits( node );
      compute_xor_block( controls, t );
    }
  }

  void compute_and( uint32_t c1, uint32_t c2, bool p1, bool p2, uint32_t t )
  {
    if ( p1 )
      qnet.add_gate( tweedledum::gate_kinds_t::pauli_x, c1 );
    if ( p2 )
      qnet.add_gate( tweedledum::gate_kinds_t::pauli_x, c2 );
    qnet.add_gate( tweedledum::gate_kinds_t::mcx, std::vector<uint32_t>{{c1, c2}},
                   std::vector<uint32_t>{{t}} );
    if ( p2 )
      qnet.add_gate( tweedledum::gate_kinds_t::pauli_x, c2 );
    if ( p1 )
      qnet.add_gate( tweedledum::gate_kinds_t::pauli_x, c1 );
  }

  void compute_or( uint32_t c1, uint32_t c2, bool p1, bool p2, uint32_t t )
  {
    if ( !p1 )
      qnet.add_gate( tweedledum::gate_kinds_t::pauli_x, c1 );
    if ( !p2 )
      qnet.add_gate( tweedledum::gate_kinds_t::pauli_x, c2 );
    qnet.add_gate( tweedledum::gate_kinds_t::mcx, std::vector<uint32_t>{{c1, c2}},
                   std::vector<uint32_t>{{t}} );
    qnet.add_gate( tweedledum::gate_kinds_t::pauli_x, t );
    if ( !p2 )
      qnet.add_gate( tweedledum::gate_kinds_t::pauli_x, c2 );
    if ( !p1 )
      qnet.add_gate( tweedledum::gate_kinds_t::pauli_x, c1 );
  }

  void compute_xor( uint32_t c1, uint32_t c2, bool inv, uint32_t t )
  {
    qnet.add_gate( tweedledum::gate_kinds_t::cx, c1, t );
    qnet.add_gate( tweedledum::gate_kinds_t::cx, c2, t );
    if ( inv )
      qnet.add_gate( tweedledum::gate_kinds_t::pauli_x, t );
  }

  void compute_xor3( uint32_t c1, uint32_t c2, uint32_t c3, bool inv, uint32_t t )
  {
    qnet.add_gate( tweedledum::gate_kinds_t::cx, c1, t );
    qnet.add_gate( tweedledum::gate_kinds_t::cx, c2, t );
    qnet.add_gate( tweedledum::gate_kinds_t::cx, c3, t );
    if ( inv )
      qnet.add_gate( tweedledum::gate_kinds_t::pauli_x, t );
  }

  void compute_maj( uint32_t c1, uint32_t c2, uint32_t c3, bool p1, bool p2, bool p3, uint32_t t )
  {
    if ( p1 )
      qnet.add_gate( tweedledum::gate_kinds_t::pauli_x, c1 );
    if ( !p2 ) /* control 2 behaves opposite */
      qnet.add_gate( tweedledum::gate_kinds_t::pauli_x, c2 );
    if ( p3 )
      qnet.add_gate( tweedledum::gate_kinds_t::pauli_x, c3 );
    qnet.add_gate( tweedledum::gate_kinds_t::cx, c1, c2 );
    qnet.add_gate( tweedledum::gate_kinds_t::cx, c3, c1 );
    qnet.add_gate( tweedledum::gate_kinds_t::cx, c3, t );
    qnet.add_gate( tweedledum::gate_kinds_t::mcx, std::vector<uint32_t>{{c1, c2}},
                   std::vector<uint32_t>{{t}} );
    qnet.add_gate( tweedledum::gate_kinds_t::cx, c3, c1 );
    qnet.add_gate( tweedledum::gate_kinds_t::cx, c1, c2 );
    if ( p3 )
      qnet.add_gate( tweedledum::gate_kinds_t::pauli_x, c3 );
    if ( !p2 )
      qnet.add_gate( tweedledum::gate_kinds_t::pauli_x, c2 );
    if ( p1 )
      qnet.add_gate( tweedledum::gate_kinds_t::pauli_x, c1 );
  }

  void compute_xor_block( std::vector<uint32_t> const& controls, uint32_t t )
  {
    for ( auto c : controls )
    {
      if ( c != t )
        qnet.add_gate( tweedledum::gate_kinds_t::cx, c, t );
    }
  }

  void compute_lut( kitty::dynamic_truth_table const& function,
                    std::vector<uint32_t> const& controls, uint32_t t )
  {
    auto qubit_map = controls;
    qubit_map.push_back( t );
    stg_fn( qnet, function, qubit_map );
  }

  void compute_xor_inplace( uint32_t c1, uint32_t c2, bool inv, uint32_t t )
  {
    if ( c1 == t )
    {
      qnet.add_gate( tweedledum::gate_kinds_t::cx, c2, c1 );
    }
    else if ( c2 == t )
    {
      qnet.add_gate( tweedledum::gate_kinds_t::cx, c1, c2 );
    }
    else
    {
      std::cerr << "[e] target does not match any control in in-place\n";
    }
    if ( inv )
      qnet.add_gate( tweedledum::gate_kinds_t::pauli_x, t );
  }

  void compute_xor3_inplace( uint32_t c1, uint32_t c2, uint32_t c3, bool inv, uint32_t t )
  {
    if ( c1 == t )
    {
      qnet.add_gate( tweedledum::gate_kinds_t::cx, c2, c1 );
      qnet.add_gate( tweedledum::gate_kinds_t::cx, c3, c1 );
    }
    else if ( c2 == t )
    {
      qnet.add_gate( tweedledum::gate_kinds_t::cx, c1, c2 );
      qnet.add_gate( tweedledum::gate_kinds_t::cx, c3, c2 );
    }
    else if ( c3 == t )
    {
      qnet.add_gate( tweedledum::gate_kinds_t::cx, c1, c3 );
      qnet.add_gate( tweedledum::gate_kinds_t::cx, c2, c3 );
    }
    else
    {
      std::cerr << "[e] target does not match any control in in-place\n";
    }
    if ( inv )
      qnet.add_gate( tweedledum::gate_kinds_t::pauli_x, t );
  }

private:
  QuantumNetwork& qnet;
  LogicNetwork const& ntk;
  SingleTargetGateSynthesisFn const& stg_fn;
  logic_network_synthesis_params const& ps;
  logic_network_synthesis_stats& st;
  mt::node_map<uint32_t, LogicNetwork> node_to_qubit;
  std::stack<uint32_t> free_ancillae;
};

} // namespace detail

/*! \brief Hierarchical synthesis based on a logic network
 *
 * This algorithm used hierarchical synthesis and computes a reversible network
 * for each gate in the circuit and computes the intermediate result to an
 * ancilla line.  The node may be computed out-of-place or in-place.  The
 * order in which nodes are computed and uncomputed, and whether they are
 * computed out-of-place or in-place is determined by a separate mapper
 * component `MappingStrategy` that is passed as template parameter to the
 * function.
 */
template<class QuantumNetwork, class LogicNetwork,
         class MappingStrategy = bennett_inplace_mapping_strategy<LogicNetwork>,
         class SingleTargetGateSynthesisFn = tweedledum::stg_from_pprm>
void logic_network_synthesis( QuantumNetwork& qnet, LogicNetwork const& ntk,
                              SingleTargetGateSynthesisFn const& stg_fn = {},
                              logic_network_synthesis_params const& ps = {},
                              logic_network_synthesis_stats* pst = nullptr )
{
  static_assert( mt::is_network_type_v<LogicNetwork>, "LogicNetwork is not a network type" );

  logic_network_synthesis_stats st;
  detail::logic_network_synthesis_impl<QuantumNetwork, LogicNetwork, MappingStrategy, SingleTargetGateSynthesisFn> impl( qnet,
                                                                                                                         ntk,
                                                                                                                         stg_fn,
                                                                                                                         ps, st );
  impl.run();
  if ( ps.verbose )
  {
    st.report();
  }

  if ( pst )
  {
    *pst = st;
  }
}

} /* namespace caterpillar */
