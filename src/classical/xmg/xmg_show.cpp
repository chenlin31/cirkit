/* CirKit: A circuit toolkit
 * Copyright (C) 2009-2015  University of Bremen
 * Copyright (C) 2015-2016  EPFL
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "xmg_show.hpp"

#include <fstream>
#include <iostream>

#include <boost/graph/graphviz.hpp>

#include <range/v3/iterator_range.hpp>
#include <range/v3/algorithm/for_each.hpp>
#include <range/v3/view/concat.hpp>
#include <range/v3/view/filter.hpp>
#include <range/v3/view/iota.hpp>
#include <range/v3/view/transform.hpp>
#include <range/v3/view/zip.hpp>

#include <core/utils/range_utils.hpp>
#include <core/utils/string_template.hpp>
#include <core/utils/string_utils.hpp>

using namespace ranges;

namespace cirkit
{

/******************************************************************************
 * Types                                                                      *
 ******************************************************************************/

struct xmg_dot_writer
{
public:
  xmg_dot_writer( xmg_graph& xmg, const properties::ptr& settings )
    : xmg( xmg )
  {
    xor_color = get( settings, "xor_color", xor_color );
    maj_color = get( settings, "maj_color", maj_color );
    and_color = get( settings, "and_color", and_color );
    or_color  = get( settings, "or_color",  or_color );
    io_color  = get( settings, "io_color",  io_color );

    show_and_or_edges = get( settings, "show_and_or_edges", show_and_or_edges );
    show_node_ids     = get( settings, "show_node_ids",     show_node_ids );
  }

  std::string get_node_label( xmg_node n, const std::string& label )
  {
    std::string str = boost::str( boost::format( "<<font point-size=\"11\">%s</font>" ) % label );
    if ( show_node_ids )
    {
      str += boost::str( boost::format( "<br/><font point-size=\"10\">%d</font>" ) % n );
    }
    str += ">";

    return str;
  }

  void operator()( std::ostream& os, xmg_node n )
  {
    string_properties_map_t properties;

    if ( xmg.is_input( n ) )
    {
      properties["style"] = "filled";
      properties["fillcolor"] = io_color;
      properties["shape"] = "house";
      properties["label"] = get_node_label( n, n == 0 ? "0" : xmg.input_name( n ) );
    }
    else if ( xmg.is_xor( n ) )
    {
      properties["style"] = "filled";
      properties["fillcolor"] = xor_color;
      properties["label"] = get_node_label( n, "XOR" );
    }
    else if ( xmg.is_maj( n ) )
    {
      properties["style"] = "filled";

      if ( *( boost::adjacent_vertices( n, xmg.graph() ).first ) != 0 )
      {
        properties["fillcolor"] = maj_color;
        properties["label"] = get_node_label( n, "MAJ" );
      }
      else
      {
        if ( xmg.complement()[*( boost::out_edges( n, xmg.graph() ).first )] )
        {
          properties["fillcolor"] = or_color;
          properties["label"] = get_node_label( n, "OR" );
        }
        else
        {
          properties["fillcolor"] = and_color;
          properties["label"] = get_node_label( n, "AND" );
        }
      }
    }

    if ( xmg.is_marked( n ) )
    {
      properties["fillcolor"] = "red";
      properties["style"] = "filled";
    }

    os << "[" << make_properties_string( properties ) << "]";
  }

  void operator()( std::ostream& os, const xmg_edge& e )
  {
    if ( !show_and_or_edges && boost::target( e, xmg.graph() ) == 0 )
    {
      os << "[style=invis]";
    }
    else if ( xmg.complement()[e] )
    {
      os << "[style=dashed]";
    }
  }

  void operator()( std::ostream& os )
  {
    /* outputs */
    auto index = 0u;
    for ( const auto& o : xmg.outputs() )
    {
      os << boost::format( "o%d[label=<<font point-size=\"11\">%s</font>>,shape=house,fillcolor=%s,style=filled];" ) % index % o.second % io_color << std::endl;
      os << boost::format( "o%d -> %d " ) % index % o.first.node;
      if ( o.first.complemented )
      {
        os << "[style=dashed]";
      }
      os << ";" << std::endl;
      ++index;
    }

    /* levels */
    xmg.compute_levels();
    auto max_level = 0u;
    for ( const auto& o : xmg.outputs() )
    {
      max_level = std::max( xmg.level( o.first.node ), max_level );
    }

    std::vector<std::vector<xmg_node>> ranks( max_level + 2u );

    for ( const auto& n : xmg.nodes() )
    {
      ranks[xmg.level(n)].push_back( n );
    }

    for ( const auto& r : ranks )
    {
      if ( r.empty() ) { continue; }
      os << "{rank = same; " << any_join( r, "; " ) << ";}" << std::endl;
    }

    os << "{rank = same;";
    for ( auto i = 0; i < xmg.outputs().size(); ++i )
    {
      os << " o" << i << ";";
    }
    os << "}" << std::endl;
  }

private:
  xmg_graph& xmg;

  std::string xor_color = "lightskyblue";
  std::string maj_color = "lightsalmon";
  std::string and_color = "lightcoral";
  std::string or_color  = "palegreen2";
  std::string io_color  = "snow2";

  bool show_and_or_edges = false;
  bool show_node_ids = false;
};

/******************************************************************************
 * Private functions                                                          *
 ******************************************************************************/

void write_dot( xmg_graph& xmg, std::ostream& os,
                const properties::ptr& settings, const properties::ptr& statistics )
{
  xmg_dot_writer writer( xmg, settings );
  write_graphviz( os, xmg.graph(), writer, writer, writer );
}

void write_javascript_cytoscape( xmg_graph& xmg, std::ostream& os,
                                 const properties::ptr& settings,
                                 const properties::ptr& statistics )
{
  /* settings */
  const auto xor_color = get( settings, "xor_color", std::string( "#87cefa") );
  const auto maj_color = get( settings, "maj_color", std::string( "#ffa07a") );
  const auto and_color = get( settings, "and_color", std::string( "#f08080") );
  const auto or_color  = get( settings, "or_color",  std::string( "#bdffa9") );
  const auto io_color  = get( settings, "io_color",  std::string( "#cccaca") );

  string_template t(
    "<!DOCTYPE>\n"
    "<html>\n"
    "  <head>\n"
    "    <title>{{ title }}</title>\n"
    "    <meta name=\"viewport\" content=\"width=device-width, user-scalable=no, initial-scale=1, maximum-scale=1\">\n\n"
    "    <script src=\"http://code.jquery.com/jquery-2.0.3.min.js\"></script>\n"
    "    <script src=\"http://cytoscape.github.io/cytoscape.js/api/cytoscape.js-latest/cytoscape.min.js\"></script>\n\n"
    "    <script src=\"https://cdn.rawgit.com/cpettitt/dagre/v0.7.4/dist/dagre.min.js\"></script>\n"
    "    <script src=\"https://cdn.rawgit.com/cytoscape/cytoscape.js-dagre/1.1.2/cytoscape-dagre.js\"></script>\n\n"
    "    <style>\n"
    "      #cy { width: 100%; height: 100%; position: absolute; left: 0; top: 0; z-index: 999 }\n"
    "    </style>\n"
    "    <script>\n"
    "      $(function(){\n"
    "        $('#cy').cytoscape({\n"
    "          layout: { name: 'dagre' },\n"
    "          boxSelectionEnabled: false,\n"
    "          autounselectify: true,\n"
    "          style: cytoscape.stylesheet()\n"
    "            .selector('node')\n"
    "              .css({\n"
    "                'content': 'data(label)',\n"
    "                'height': '25',\n"
    "                'width': '25',\n"
    "                'text-valign': 'center',\n"
    "                'text-outline-width': 2,\n"
    "                'color': '#fff'\n"
    "              })\n"
    "            .selector('node.and')\n"
    "              .css({\n"
    "                'text-outline-color': '{{ and_color }}',\n"
    "                'background-color': '{{ and_color }}'\n"
    "              })\n"
    "            .selector('node.or')\n"
    "              .css({\n"
    "                'text-outline-color': '{{ or_color }}',\n"
    "                'background-color': '{{ or_color }}'\n"
    "              })\n"
    "            .selector('node.maj')\n"
    "              .css({\n"
    "                'text-outline-color': '{{ maj_color }}',\n"
    "                'background-color': '{{ maj_color }}'\n"
    "              })\n"
    "            .selector('node.xor')\n"
    "              .css({\n"
    "                'text-outline-color': '{{ xor_color }}',\n"
    "                'background-color': '{{ xor_color }}'\n"
    "              })\n"
    "            .selector('node.pi')\n"
    "              .css({\n"
    "                'shape': 'triangle',\n"
    "                'text-outline-color': '{{ io_color }}',\n"
    "                'background-color': '{{ io_color }}'\n"
    "              })\n"
    "            .selector('node.po')\n"
    "              .css({\n"
    "                'shape': 'triangle',\n"
    "                'text-outline-color': '{{ io_color }}',\n"
    "                'background-color': '{{ io_color }}'\n"
    "              })\n"
    "            .selector('edge')\n"
    "              .css({\n"
    "                'width': '1',\n"
    "                'source-arrow-shape': 'triangle',\n"
    "                'curve-style': 'bezier'\n"
    "              })\n"
    "            .selector('edge.complemented')\n"
    "              .css({\n"
    "                'line-style': 'dotted'\n"
    "              }),\n"
    "          elements: {\n"
    "            nodes: [\n{{ nodes }}"
    "            ],\n"
    "            edges: [\n{{ edges }}"
    "            ]\n"
    "          }\n"
    "        });\n"
    "      });\n"
    "    </script>\n"
    "  </head>\n"
    "  <div id=\"cy\"></div>\n"
    "</html>\n" );

  std::string nodes;
  const auto node_transform = [&xmg]( xmg_node n ) {
    std::string type = xmg.is_input( n ) ? "pi" : ( xmg.is_maj( n ) ? ( xmg.is_pure_maj( n ) ? "maj" : ( xmg.children( n )[0].complemented ? "or" : "and" ) ) : "xor" );
    std::string label = xmg.is_input( n ) ? ( n == 0 ? "0" : xmg.input_name( n )  ) : type;

    return boost::str( boost::format( "            { data: { id: 'n%1%', type: '%2%', label: '%3%' }, classes: '%2%' },\n" ) % n % type % label );
  };
  const auto output_transform = []( std::pair<std::pair<xmg_function, std::string>, unsigned> t ) {
    return boost::str( boost::format( "            { data: { id: 'o%1%', type: 'po', label: '%2%' }, classes: 'po' },\n" ) % t.second % t.first.second );
  };
  const auto output_pairs = view::zip( xmg.outputs(), view::ints( 0u, static_cast<unsigned>( xmg.outputs().size() ) ) );
  const auto outputs = view::transform( output_pairs, output_transform );
  for_each( view::concat( view::transform( xmg.nodes(), node_transform ), outputs ), [&nodes]( const std::string& s ) { nodes += s; } );

  std::string edges;
  const auto edge_filter = [&xmg]( const xmg_edge& e ) { return boost::target( e, xmg.graph() ) > 0; };
  const auto edge_transform = [&xmg]( const xmg_edge& e ) {
    std::string classes;

    if ( xmg.complement()[e] )
    {
      classes += " complemented";
    }
    return boost::str( boost::format( "            { data: { source: 'n%d', target: 'n%d' }, classes: '%s' },\n" ) % boost::source( e, xmg.graph() ) % boost::target( e, xmg.graph() ) % classes );
  };
  const auto output_edges_transform = []( std::pair<std::pair<xmg_function, std::string>, unsigned> t ) {
    return boost::str( boost::format( "            { data: { source: 'o%d', target: 'n%d' }, classes: '%s' },\n" ) % t.second % t.first.first.node % ( t.first.first.complemented ? "complemented" : "" ) );
  };
  const auto output_edges = view::transform( output_pairs, output_edges_transform );
  for_each( view::concat( view::transform( view::filter( xmg.edges(), edge_filter ), edge_transform ), output_edges ), [&edges]( const std::string& s ) { edges += s; }  );

  os << t( {
      {"xor_color", xor_color},
      {"maj_color", maj_color},
      {"and_color", and_color},
      {"or_color", or_color},
      {"io_color", io_color},
      {"title", xmg.name()},
      {"nodes", nodes},
      {"edges", edges}
    } );
}

/******************************************************************************
 * Public functions                                                           *
 ******************************************************************************/

void write_dot( xmg_graph& xmg, const std::string& filename,
                const properties::ptr& settings, const properties::ptr& statistics )
{
  std::ofstream os( filename.c_str(), std::ofstream::out );
  write_dot( xmg, os, settings, statistics );
}

void write_javascript_cytoscape( xmg_graph& xmg, const std::string& filename,
                                 const properties::ptr& settings,
                                 const properties::ptr& statistics )
{
  std::ofstream os( filename.c_str(), std::ofstream::out );
  write_javascript_cytoscape( xmg, os, settings, statistics );
}

}

// Local Variables:
// c-basic-offset: 2
// eval: (c-set-offset 'substatement-open 0)
// eval: (c-set-offset 'innamespace 0)
// End: