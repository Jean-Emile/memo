#include <infinit/smb/smb.hh>
#include <infinit/model/doughnut/Doughnut.hh>
#include <infinit/filesystem/filesystem.hh>

#include <elle/serialization/json.hh>

#include <reactor/scheduler.hh>

ELLE_LOG_COMPONENT("infinit-smb");

#include <main.hh>

using boost::program_options::variables_map;

infinit::Infinit ifnt;

COMMAND(run)
{
  auto name = mandatory(args, "name", "network name");
  auto self = self_user(ifnt, args);
  auto network = ifnt.network_get(name, self);
  bool cache = flag(args, option_cache);
  auto cache_ram_size = optional<int>(args, option_cache_ram_size);
  auto cache_ram_ttl = optional<int>(args, option_cache_ram_ttl);
  auto cache_ram_invalidation =
    optional<int>(args, option_cache_ram_invalidation);
  report_action("running", "network", network.name);
  auto model = network.run(
    self,
    {},
    true,
    cache,
    cache_ram_size, cache_ram_ttl, cache_ram_invalidation, flag(args, "async"));
  if (aliased_flag(args, {"fetch-endpoints", "fetch"}))
  {
    infinit::model::NodeLocations hosts;
    beyond_fetch_endpoints(network, hosts);
    model->overlay()->discover(hosts);
  }
  auto fs = elle::make_unique<infinit::filesystem::FileSystem>(
    args["volume"].as<std::string>(),
    std::shared_ptr<infinit::model::doughnut::Doughnut>(model.release()));
  new infinit::smb::SMBServer(std::move(fs));
  reactor::sleep();
}

int
main(int argc, char** argv)
{
  using boost::program_options::value;
  using boost::program_options::bool_switch;
  Modes modes {
    {
      "run",
      "Run",
      &run,
      "--name NETWORK",
      {
        { "name,n", value<std::string>(), "network name" },
        { "volume", value<std::string>(), "volume name" },
        { "peer", value<std::vector<std::string>>()->multitoken(),
            "peer to connect to (host:port)" },
        { "async", bool_switch(), "use asynchronous operations" },
        option_cache,
        option_cache_ram_size,
        option_cache_ram_ttl,
        option_cache_ram_invalidation,
        { "fetch-endpoints", bool_switch(),
          elle::sprintf("fetch endpoints from %s", beyond()).c_str() },
        { "fetch,f", bool_switch(), "alias for --fetch-endpoints" },
      },
    },
  };
  return infinit::main("Infinit SMB adapter", modes, argc, argv);
}
