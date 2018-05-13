/* CirKit: A circuit toolkit
 * Copyright (C) 2009-2015  University of Bremen
 * Copyright (C) 2015-2017  EPFL
 *
 * Permission is hereby granted, free of charge, to any person
 * obtaining a copy of this software and associated documentation
 * files (the "Software"), to deal in the Software without
 * restriction, including without limitation the rights to use,
 * copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following
 * conditions:
 *
 * The above copyright notice and this permission notice shall be
 * included in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES
 * OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
 * NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT
 * HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY,
 * WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR
 * OTHER DEALINGS IN THE SOFTWARE.
 */

#include "xmgmine.hpp"

#include <alice/rules.hpp>
#include <cli/stores.hpp>
#include <formal/xmg/xmg_mine.hpp>
#include <formal/xmg/xmg_minlib.hpp>

#include <fmt/format.h>

namespace cirkit
{

xmgmine_command::xmgmine_command( const environment::ptr& env )
  : cirkit_command( env, "Mine optimum XMGs" )
{
  add_option( "--lut_file", lut_file, "filename with truth table in binary form in each line" );
  add_option( "--opt_file", opt_file, "filename with optimum XMG database" )->check( CLI::ExistingFile );
  add_option( "--timeout,-t", timeout, "timeout in seconds (afterwards, heuristics are tried)" );
  add_flag( "--add,-a", "add current XMG to database" );
  add_flag( "--verify", "verifies entries in optimum XMG database" );
  be_verbose();
}

command::rules xmgmine_command::validity_rules() const
{
  return {
    {[this]() { return is_set( "verify" ) || is_set( "add" ) || is_set( "lut_file" ); }, "lut_file or verify needs to be set" },
    {[this]() { return is_set( "verify" ) || is_set( "add" ) || std::ifstream( lut_file.c_str() ).good(); }, "lut_file does not exist" },
    {[this]() { return !is_set( "add" ) || env->store<xmg_graph>().current_index() != -1; }, "no XMG in store" },
    {[this]() { return !is_set( "add" ) || env->store<xmg_graph>().current().outputs().size() == 1u; }, "XMG can only have one output" }
  };
}

void xmgmine_command::execute()
{
  /* derive opt_file */
  if ( !is_set( "opt_file" ) )
  {
    opt_file.clear();
    if ( const auto* path = std::getenv( "CIRKIT_HOME" ) )
    {
      const auto filename = fmt::format( "{}/xmgmin.txt", path );
      if ( std::ifstream( filename.c_str() ).good() )
      {
        opt_file = filename;
      }
    }
  }

  if ( opt_file.empty() )
  {
    std::cout << "[e] cannot find optimum XMG database" << std::endl;
  }

  /* mine or verify */
  if ( is_set( "verify" ) )
  {
    xmg_minlib_manager minlib( make_settings() );
    minlib.load_library_file( opt_file );

    if ( !minlib.verify() )
    {
      std::cout << "[w] minlib verification failed" << std::endl;
    }
    else
    {
      std::cout << "[i] minlib verification succeeded" << std::endl;
    }
  }
  else if ( is_set( "add" ) )
  {
    const auto& xmgs = env->store<xmg_graph>();

    xmg_minlib_manager minlib( make_settings() );
    minlib.load_library_file( opt_file );
    minlib.add_to_library( xmgs.current() );
    minlib.write_library_file( opt_file, 5u );
  }
  else
  {
    const auto settings = make_settings();
    if ( is_set( "timeout" ) )
    {
      settings->set( "timeout", boost::optional<unsigned>( timeout ) );
    }
    xmg_mine( lut_file, opt_file, settings );
  }
}

}

// Local Variables:
// c-basic-offset: 2
// eval: (c-set-offset 'substatement-open 0)
// eval: (c-set-offset 'innamespace 0)
// End:
