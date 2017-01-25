#include <infinit/cli/Doctor.hh>

#include <numeric> // iota
#include <regex>

#include <boost/algorithm/cxx11/all_of.hpp>
#include <boost/algorithm/cxx11/any_of.hpp>
#include <boost/algorithm/string/predicate.hpp>
#include <boost/range/algorithm/equal.hpp>
#include <boost/range/algorithm/find_if.hpp>
#include <boost/range/algorithm/sort.hpp>
#include <boost/range/algorithm_ext/iota.hpp>

#include <elle/bytes.hh>
#include <elle/filesystem/TemporaryDirectory.hh>
#include <elle/filesystem/path.hh>
#include <elle/log.hh>
#include <elle/network/Interface.hh>
#include <elle/os/environ.hh>
#include <elle/string/algorithm.hh> // elle::join
#include <elle/system/Process.hh>

#include <cryptography/random.hh>

#include <reactor/connectivity/connectivity.hh>
#include <reactor/filesystem.hh>
#include <reactor/network/upnp.hh>
#include <reactor/scheduler.hh>
#include <reactor/TimeoutGuard.hh>
#include <reactor/http/exceptions.hh>

#include <infinit/cli/Infinit.hh>
#include <infinit/storage/Dropbox.hh>
#include <infinit/storage/Filesystem.hh>
#include <infinit/storage/GCS.hh>
#include <infinit/storage/GoogleDrive.hh>
#include <infinit/storage/Strip.hh>
#include <cryptography/random.hh>
#ifndef INFINIT_WINDOWS
# include <infinit/storage/sftp.hh>
#endif
#include <infinit/storage/S3.hh>

ELLE_LOG_COMPONENT("cli.doctor");

#include <infinit/cli/doctor-networking.hh>

namespace infinit
{
  using Passport = infinit::model::doughnut::Passport;
  namespace cli
  {
    using Error = das::cli::Error;
    namespace bfs = boost::filesystem;

#include <infinit/cli/doctor-utility.hh>

    std::string const Doctor::connectivity_server = "connectivity.infinit.sh";

    Doctor::Doctor(Infinit& infinit)
      : Object(infinit)
      , all(
        "Perform all possible checks",
        das::cli::Options(),
        this->bind(modes::mode_all,
                   cli::ignore_non_linked = false,
                   cli::upnp_tcp_port = boost::none,
                   cli::upnp_udt_port = boost::none,
                   cli::server = connectivity_server,
                   cli::no_color = false,
                   cli::verbose = false))
      , configuration(
        "Perform integrity checks on the Infinit configuration files",
        das::cli::Options(),
        this->bind(modes::mode_configuration,
                   cli::ignore_non_linked = false,
                   cli::no_color = false,
                   cli::verbose = false))
      , connectivity(
        "Perform connectivity checks",
        das::cli::Options(),
        this->bind(modes::mode_connectivity,
                   cli::upnp_tcp_port = boost::none,
                   cli::upnp_udt_port = boost::none,
                   cli::server = connectivity_server,
                   cli::no_color = false,
                   cli::verbose = false))
      , networking(
        "Perform networking speed tests between nodes",
        das::cli::Options(),
        this->bind(modes::mode_networking,
                   cli::mode = boost::none,
                   cli::protocol = boost::none,
                   cli::packet_size = boost::none,
                   cli::packets_count = boost::none,
                   cli::host = boost::none,
                   cli::port = boost::none,
                   cli::tcp_port = boost::none,
                   cli::utp_port = boost::none,
                   cli::xored_utp_port = boost::none,
                   cli::xored = boost::none,
                   cli::no_color = false,
                   cli::verbose = false))
      , system(
        "Perform sanity checks on your system",
        das::cli::Options(),
        this->bind(modes::mode_system,
                   cli::no_color = false,
                   cli::verbose = false))
    {}


    /*------------.
    | Mode: all.  |
    `------------*/

    void
    Doctor::mode_all(bool ignore_non_linked,
                     boost::optional<uint16_t> upnp_tcp_port,
                     boost::optional<uint16_t> upnp_udt_port,
                     boost::optional<std::string> const& server,
                     bool no_color,
                     bool verbose)
    {
      ELLE_TRACE_SCOPE("all");
      auto& cli = this->cli();

      auto results = All{};
      _system_sanity(cli, results.system_sanity);
      _configuration_integrity(cli, ignore_non_linked, results.configuration_integrity);
      _connectivity(cli,
                    server,
                    upnp_tcp_port,
                    upnp_udt_port,
                    results.connectivity);
      _output(cli, std::cout, results, no_color, verbose);
      _report_error(cli, std::cout, results.sane(), results.warning());
    }

    /*----------------------.
    | Mode: configuration.  |
    `----------------------*/

    void
    Doctor::mode_configuration(bool ignore_non_linked,
                               bool no_color,
                               bool verbose)
    {
      ELLE_TRACE_SCOPE("configuration");
      auto& cli = this->cli();

      auto results = ConfigurationIntegrityResults{};
      _configuration_integrity(cli, ignore_non_linked, results);
      _output(cli, std::cout, results, no_color, verbose);
      _report_error(cli, std::cout, results.sane(), results.warning());
    }



    /*---------------------.
    | Mode: connectivity.  |
    `---------------------*/

    void
    Doctor::mode_connectivity(boost::optional<uint16_t> upnp_tcp_port,
                              boost::optional<uint16_t> upnp_udt_port,
                              boost::optional<std::string> const& server,
                              bool no_color,
                              bool verbose)
    {
      ELLE_TRACE_SCOPE("connectivity");
      auto& cli = this->cli();

      auto results = ConnectivityResults{};
      _connectivity(cli,
                    server,
                    upnp_tcp_port,
                    upnp_udt_port,
                    results);
      _output(cli, std::cout, results, no_color, verbose);
      _report_error(cli, std::cout, results.sane(), results.warning());
    }


    /*-------------------.
    | Mode: networking.  |
    `-------------------*/

    void
    Doctor::mode_networking(boost::optional<std::string> const& mode_name,
                            boost::optional<std::string> const& protocol_name,
                            boost::optional<elle::Buffer::Size> packet_size,
                            boost::optional<int64_t> packets_count,
                            boost::optional<std::string> const& host,
                            boost::optional<uint16_t> port,
                            boost::optional<uint16_t> tcp_port,
                            boost::optional<uint16_t> utp_port,
                            boost::optional<uint16_t> xored_utp_port,
                            boost::optional<std::string> const& xored,
                            bool no_color,
                            bool verbose)
    {
      ELLE_TRACE_SCOPE("networking");
      auto& cli = this->cli();

      auto v = cli.compatibility_version().value_or(infinit::version());
      if (host)
      {
        elle::printf("Client mode (version: %s):", v) << std::endl;
        infinit::networking::perform(mode_name,
                                     protocol_name,
                                     packet_size,
                                     packets_count,
                                     *host,
                                     port,
                                     tcp_port,
                                     utp_port,
                                     xored_utp_port,
                                     xored,
                                     verbose,
                                     v);
      }
      else
      {
        elle::printf("Server mode (version: %s):", v) << std::endl;
        auto servers = infinit::networking::Servers(protocol_name,
                                                    port,
                                                    tcp_port,
                                                    utp_port,
                                                    xored_utp_port,
                                                    xored,
                                                    verbose,
                                                    v);
        reactor::sleep();
      }
    }


    /*---------------.
    | Mode: system.  |
    `---------------*/

    void
    Doctor::mode_system(bool no_color, bool verbose)
    {
      ELLE_TRACE_SCOPE("system");
      auto& cli = this->cli();

      auto results = SystemSanityResults{};
      _system_sanity(cli, results);
      _output(cli, std::cout, results, no_color, verbose);
      _report_error(cli, std::cout, results.sane(), results.warning());
    }
  }
}
