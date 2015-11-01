#include <elle/log.hh>
#include <elle/serialization/json.hh>

#include <reactor/FDStream.hh>

#include <infinit/model/doughnut/Doughnut.hh>
#include <infinit/model/doughnut/Local.hh>
#include <infinit/overlay/kelips/Kelips.hh>
#include <infinit/storage/Storage.hh>

ELLE_LOG_COMPONENT("infinit-volume");

#include <main.hh>

using namespace boost::program_options;
options_description mode_options("Modes");

infinit::Infinit ifnt;

static
std::string
volume_name(variables_map const& args, infinit::User const& owner)
{
  return ifnt.qualified_name(mandatory(args, "name", "volume name"), owner);
}

static
void
create(variables_map const& args)
{
  auto owner = self_user(ifnt, args);
  auto name = volume_name(args, owner);
  auto mountpoint = optional(args, "mountpoint");
  auto network = ifnt.network_get(mandatory(args, "network"), owner);
  ELLE_TRACE("start network");
  report_action("starting", "network", network.name, std::string("locally"));
  std::vector<std::string> hosts;
  if (args.count("peer"))
    hosts = args["peer"].as<std::vector<std::string>>();
  auto model = network.run(hosts);
  ELLE_TRACE("create volume");
  auto fs = elle::make_unique<infinit::filesystem::FileSystem>(name, model.second);
  infinit::Volume volume(name, mountpoint, network.name);
  if (args.count("stdout") && args["stdout"].as<bool>())
  {
    elle::serialization::json::SerializerOut s(std::cout, false);
    s.serialize_forward(volume);
  }
  else
  {
    report_created("volume", name);
    ifnt.volume_save(volume);
  }
  if (args.count("push") && args["push"].as<bool>())
    beyond_push("volume", name, volume, owner);
}

static
void
export_(variables_map const& args)
{
  auto owner = self_user(ifnt, args);
  auto name = volume_name(args, owner);
  auto output = get_output(args);
  auto volume = ifnt.volume_get(name);
  volume.mountpoint.reset();
  {
    elle::serialization::json::SerializerOut s(*output, false);
    s.serialize_forward(volume);
  }
  report_exported(*output, "volume", volume.name);
}

static
void
import(variables_map const& args)
{
  auto input = get_input(args);
  elle::serialization::json::SerializerIn s(*input, false);
  infinit::Volume volume(s);
  volume.mountpoint = optional(args, "mountpoint");
  ifnt.volume_save(volume);
  report_imported("volume", volume.name);
}

static
void
push(variables_map const& args)
{
  auto owner = self_user(ifnt, args);
  auto name = volume_name(args, owner);
  auto volume = ifnt.volume_get(name);
  // Don't push the mountpoint to beyond.
  volume.mountpoint = boost::none;
  auto network = ifnt.network_get(volume.network, owner);
  auto owner_uid = infinit::User::uid(network.dht()->owner);
  beyond_push("volume", name, volume, owner);
}

static void
pull(variables_map const& args)
{
  auto owner = self_user(ifnt, args);
  auto name = volume_name(args, owner);
  beyond_delete("volume", name, owner);
}

static void
delete_(variables_map const& args)
{
  auto owner = self_user(ifnt, args);
  auto name = volume_name(args, owner);
  auto path = ifnt._volume_path(name);
  if (boost::filesystem::remove(path))
    report_action("deleted", "volume", name, std::string("locally"));
  else
    throw elle::Error(
      elle::sprintf("File for volume could not be deleted: %s", path));
}

static
void
fetch(variables_map const& args)
{
  auto owner = self_user(ifnt, args);
  auto network_name_ = optional(args, "network");
  if (optional(args, "name"))
  {
    auto name = volume_name(args, owner);
    auto desc = beyond_fetch<infinit::Volume>("volume", name);
    ifnt.volume_save(std::move(desc));
  }
  else if (network_name_) // Fetch all networks for network.
  {
    std::string network_name = ifnt.qualified_name(network_name_.get(), owner);
    auto res = beyond_fetch<
      std::unordered_map<std::string, std::vector<infinit::Volume>>>(
        elle::sprintf("networks/%s/volumes", network_name),
        "volumes for network",
        network_name);
    for (auto const& volume: res["volumes"])
      ifnt.volume_save(std::move(volume));
  }
  else // Fetch all networks for owner.
  {
    auto res = beyond_fetch<
      std::unordered_map<std::string, std::vector<infinit::Volume>>>(
        elle::sprintf("users/%s/volumes", owner.name),
        "volumes for user",
        owner.name,
        owner);
    for (auto const& volume: res["volumes"])
    {
      try
      {
        ifnt.volume_save(std::move(volume));
      }
      catch (ResourceAlreadyFetched const& error)
      {
      }
    }
  }
}

static
void
run(variables_map const& args)
{
  auto self = self_user(ifnt, args);
  auto name = volume_name(args, self);
  std::vector<std::string> hosts;
  if (args.count("peer"))
    hosts = args["peer"].as<std::vector<std::string>>();
  auto volume = ifnt.volume_get(name);
  auto network = ifnt.network_get(volume.network, self);
  ELLE_TRACE("run network");
  bool cache = args.count("cache");
  boost::optional<int> cache_size(0); // Not initializing warns on GCC 4.9
  if (args.count("cache") && args["cache"].as<int>() != 0)
    cache_size = args["cache"].as<int>();
  else
    cache_size.reset();
  bool async_writes =
    args.count("async-writes") && args["async-writes"].as<bool>();
  reactor::scheduler().signal_handle(
    SIGINT,
    [&]
    {
      ELLE_TRACE("terminating");
      reactor::scheduler().terminate();
    });
  bool push = args.count("push") && args["push"].as<bool>();
  bool fetch = args.count("fetch") && args["fetch"].as<bool>();
  if (fetch)
    beyond_fetch_endpoints(network, hosts);
  report_action("running", "network", network.name);
  auto model = network.run(hosts, true, cache, cache_size, async_writes,
    args.count("async") && args["async"].as<bool>(),
    args.count("cache-model") && args["cache-model"].as<bool>());
  auto run = [&]
  {
    ELLE_TRACE_SCOPE("run volume");
    report_action("running", "volume", volume.name);
    auto fs = volume.run(model.second, optional(args, "mountpoint"));
    elle::SafeFinally unmount([&]
    {
      ELLE_TRACE("unmounting")
        fs->unmount();
    });
    if (script_mode)
    {
      reactor::FDStream stdin(0);
      while (true)
      {
        try
        {
          auto json =
            boost::any_cast<elle::json::Object>(elle::json::read(stdin));
          ELLE_TRACE("got command: %s", json);
          elle::serialization::json::SerializerIn command(json, false);
          auto op = command.deserialize<std::string>("operation");
          auto path =
            fs->path(command.deserialize<std::string>("path"));
          if (op == "list_directory")
          {
            std::vector<std::string> entries;
            path->list_directory(
              [&] (std::string const& path, struct stat*)
              {
                entries.push_back(path);
              });
            elle::serialization::json::SerializerOut response(std::cout);
            response.serialize("entries", entries);
            response.serialize("success", true);
            continue;
          }
          else if (op == "mkdir")
          {
            path->mkdir(0777);
          }
          else
            throw elle::Error(elle::sprintf("operation %s does not exist", op));
          elle::serialization::json::SerializerOut response(std::cout);
          response.serialize("success", true);
        }
        catch (reactor::FDStream::EOF const&)
        {
          return;
        }
        catch (elle::Error const& e)
        {
          elle::serialization::json::SerializerOut response(std::cout);
          response.serialize("success", false);
          response.serialize("message", e.what());
        }
      }
    }
    else
    {
      ELLE_TRACE("wait filesystem");
      reactor::wait(*fs);
    }
  };
  if (push && model.first)
  {
    elle::With<InterfacePublisher>(
      network, self, model.second->overlay()->node_id(),
      model.first->server_endpoint().port()) << [&]
    {
      run();
    };
  }
  else
    run();
}

static
void
list(variables_map const& args)
{
  for (auto const& volume: ifnt.volumes_get())
  {
    std::cout << volume.name << " : network " << volume.network;
    if (volume.mountpoint)
      std::cout << " on " << *volume.mountpoint;
    std::cout << std::endl;
  }
}

int
main(int argc, char** argv)
{
  program = argv[0];
  Modes modes {
    {
      "create",
      "Create a volume",
      &create,
      "--name VOLUME --network NETWORK [--mountpoint PATH]",
      {
        { "name", value<std::string>(), "created volume name" },
        { "network", value<std::string>(), "underlying network to use" },
        { "mountpoint", value<std::string>(), "where to mount the filesystem" },
        option_owner,
        { "stdout", bool_switch(), "output configuration to stdout" },
        { "peer", value<std::vector<std::string>>()->multitoken(),
          "peer to connect to (host:port)" },
        { "push", bool_switch(),
          elle::sprintf("push the volume to %s", beyond()).c_str() },
      },
    },
    {
      "export",
      "Export a volume for someone else to import",
      &export_,
      "--name VOLUME",
      {
        { "name,n", value<std::string>(), "network to export" },
        { "output,o", value<std::string>(),
          "file to write volume to  (stdout by default)"},
        option_owner,
      },
    },
    {
      "fetch",
      elle::sprintf("fetch volume from %s", beyond()).c_str(),
      &fetch,
      "--name NETWORK",
      {
        { "name,n", value<std::string>(), "volume to fetch" },
        { "network", value<std::string>(), "network to fetch volumes for" },
        option_owner,
      },
    },
    {
      "import",
      "Import a volume",
      &import,
      "",
      {
        { "input,i", value<std::string>(),
          "file to read volume from (defaults to stdin)" },
        { "mountpoint", value<std::string>(), "where to mount the filesystem" },
      },
    },
    {
      "push",
      elle::sprintf("Push a volume to %s", beyond()).c_str(),
      &push,
      "--name VOLUME",
      {
        { "name,n", value<std::string>(), "volume to push" },
        option_owner,
      },
    },
    {
      "run",
      "Run a volume",
      &run,
      "--name VOLUME [--mountpoint PATH]",
      {
        { "async-writes,a", bool_switch(),
          "do not wait for writes on the backend" },
        { "cache,c", value<int>()->implicit_value(0),
          "enable storage caching, "
          "optional arguments specifies maximum size in bytes" },
        { "fetch", bool_switch(),
            elle::sprintf("fetch endpoints from %s", beyond()).c_str() },
        { "mountpoint,m", value<std::string>(),
          "where to mount the filesystem" },
        { "name", value<std::string>(), "volume name" },
        { "peer", value<std::vector<std::string>>()->multitoken(),
          "peer to connect to (host:port)" },
        { "push", bool_switch(),
            elle::sprintf("push endpoints to %s", beyond()).c_str() },
        { "async", bool_switch(), "Use asynchronious operations" },
        { "cache-model", bool_switch(), "Enable model caching" },
        option_owner,
      },
    },
    {
      "delete",
      "Delete a volume",
      &delete_,
      "--name VOLUME",
      {
        { "name,n", value<std::string>(), "volume to delete" },
        option_owner,
      },
    },
    {
      "pull",
      elle::sprintf("Remove a volume from %s", beyond()).c_str(),
      &pull,
      "--name VOLUME",
      {
        { "name,n", value<std::string>(), "volume to remove" },
        option_owner,
      },
    },
    {
      "list",
      "List volumes",
      &list,
      {},
      {
        option_owner,
      }
    },
  };
  return infinit::main("Infinit volume management utility", modes, argc, argv);
}
