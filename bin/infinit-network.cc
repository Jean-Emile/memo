// http://opensource.apple.com/source/mDNSResponder/mDNSResponder-576.30.4/mDNSPosix/PosixDaemon.c
#if __APPLE__
# define daemon yes_we_know_that_daemon_is_deprecated_in_os_x_10_5_thankyou
#endif

#include <memory>

#include <elle/log.hh>
#include <elle/serialization/json.hh>
#include <elle/json/exceptions.hh>

#include <reactor/FDStream.hh>

#include <infinit/model/doughnut/Doughnut.hh>
#include <infinit/overlay/Kalimero.hh>
#include <infinit/overlay/kelips/Kelips.hh>
#include <infinit/overlay/kademlia/kademlia.hh>
#include <infinit/storage/Storage.hh>
#include <infinit/storage/Strip.hh>

ELLE_LOG_COMPONENT("infinit-network");

#include <main.hh>
#include <xattrs.hh>

infinit::Infinit ifnt;

#include <endpoint_file.hh>

#if __APPLE__
# undef daemon
extern int daemon(int, int);
#endif

static
std::unique_ptr<infinit::storage::StorageConfig>
storage_configuration(boost::program_options::variables_map const& args)
{
  std::unique_ptr<infinit::storage::StorageConfig> storage;
  auto storage_count = args.count("storage");
  if (storage_count > 0)
  {
    auto storages = args["storage"].as<std::vector<std::string>>();
    std::vector<std::unique_ptr<infinit::storage::StorageConfig>> backends;
    for (auto const& storage: storages)
      backends.emplace_back(ifnt.storage_get(storage));
    if (backends.size() == 1)
      storage = std::move(backends[0]);
    else
    {
      storage.reset(
        new infinit::storage::StripStorageConfig(std::move(backends)));
    }
  }
  return storage;
}

COMMAND(create)
{
  auto name = mandatory(args, "name", "network name");
  auto owner = self_user(ifnt, args);
  std::unique_ptr<infinit::overlay::Configuration> overlay_config;
  int overlays =
    + (args.count("kalimero") ? 1 : 0)
    + (args.count("kelips") ? 1 : 0)
  ;
  if (overlays > 1)
    throw CommandLineError("Only one overlay type must be specified");
  if (args.count("kalimero"))
  {
    overlay_config.reset(new infinit::overlay::KalimeroConfiguration());
  }
  else // default to Kelips
  {
    auto kelips =
      elle::make_unique<infinit::overlay::kelips::Configuration>();
    if (args.count("k"))
      kelips->k = args["k"].as<int>();
    else if (args.count("nodes"))
    {
      int nodes = args["nodes"].as<int>();
      if (nodes < 10)
        kelips->k = 1;
      else if (sqrt(nodes) < 5)
        kelips->k = nodes / 5;
      else
        kelips->k = sqrt(nodes);
    }
    else
      kelips->k = 1;
    if (auto timeout = optional<std::string>(args, "kelips-contact-timeout"))
    {

      kelips->contact_timeout_ms =
        std::chrono::duration_from_string<std::chrono::milliseconds>(*timeout)
        .count();
    }
    if (args.count("encrypt"))
    {
      std::string enc = args["encrypt"].as<std::string>();
      if (enc == "no")
      {
        kelips->encrypt = false;
        kelips->accept_plain = true;
      }
      else if (enc == "lazy")
      {
        kelips->encrypt = true;
        kelips->accept_plain = true;
      }
      else if (enc == "yes")
      {
        kelips->encrypt = true;
        kelips->accept_plain = false;
      }
      else
        throw CommandLineError("'encrypt' must be 'no', 'lazy' or 'yes'");
    }
    else
    {
      kelips->encrypt = true;
      kelips->accept_plain = false;
    }
    if (args.count("protocol"))
    {
      std::string proto = args["protocol"].as<std::string>();
      try
      {
        kelips->rpc_protocol = elle::serialization::Serialize<
          infinit::model::doughnut::Protocol>::convert(proto);
      }
      catch (elle::serialization::Error const& e)
      {
        throw CommandLineError("'protocol' must be 'utp', 'tcp' or 'all'");
      }
    }
    overlay_config = std::move(kelips);
  }
  auto storage = storage_configuration(args);
  // Consensus
  std::unique_ptr<
    infinit::model::doughnut::consensus::Configuration> consensus_config;
  {
    int replication_factor = 1;
    if (args.count("replication-factor"))
      replication_factor = args["replication-factor"].as<int>();
    if (replication_factor < 1)
      throw CommandLineError("replication factor must be greater than 0");
    auto eviction = optional<std::string>(args, "eviction-delay");
    bool no_consensus = args.count("no-consensus");
    bool paxos = args.count("paxos");
    if (!no_consensus)
      paxos = true;
    if (!one(no_consensus, paxos))
      throw CommandLineError("more than one consensus specified");
    if (paxos)
    {
      consensus_config = elle::make_unique<
        infinit::model::doughnut::consensus::Paxos::Configuration>(
          replication_factor,
          eviction ?
          std::chrono::duration_from_string<std::chrono::seconds>(*eviction) :
          std::chrono::seconds(10 * 60));
    }
    else
    {
      if (replication_factor != 1)
      {
        throw elle::Error(
          "without consensus, replication factor must be 1");
      }
      consensus_config = elle::make_unique<
        infinit::model::doughnut::consensus::Configuration>();
    }
  }
  infinit::model::doughnut::AdminKeys admin_keys;
  if (args.count("admin-r"))
  {
    auto admins = args["admin-r"].as<std::vector<std::string>>();
    for (auto const& a: admins)
      admin_keys.r.push_back(ifnt.user_get(a).public_key);
  }
  if (args.count("admin-rw"))
  {
    auto admins = args["admin-rw"].as<std::vector<std::string>>();
    for (auto const& a: admins)
      admin_keys.w.push_back(ifnt.user_get(a).public_key);
  }
  boost::optional<int> port;
  if (args.count("port"))
    port = args["port"].as<int>();
  auto dht =
    elle::make_unique<infinit::model::doughnut::Configuration>(
      infinit::model::Address::random(0), // FIXME
      std::move(consensus_config),
      std::move(overlay_config),
      std::move(storage),
      owner.keypair(),
      std::make_shared<infinit::cryptography::rsa::PublicKey>(owner.public_key),
      infinit::model::doughnut::Passport(
        owner.public_key,
        ifnt.qualified_name(name, owner),
        infinit::cryptography::rsa::KeyPair(owner.public_key,
                                            owner.private_key.get())),
      owner.name,
      std::move(port),
      version,
      admin_keys);
  {
    infinit::Network network(ifnt.qualified_name(name, owner), std::move(dht));
    if (args.count("output"))
    {
      auto output = get_output(args);
      elle::serialization::json::serialize(network, *output, false);
    }
    else
    {
      ifnt.network_save(owner, network);
      report_created("network", network.name);
    }
    infinit::NetworkDescriptor desc(std::move(network));
    if (!args.count("output"))
      ifnt.network_save(desc);
    if (aliased_flag(args, {"push-network", "push"}))
      beyond_push("network", desc.name, desc, owner);
  }
}
static std::pair<infinit::cryptography::rsa::PublicKey, bool>
user_key(std::string name, boost::optional<std::string> mountpoint)
{
  bool is_group = false;
  if (!name.empty() && name[0] == '@')
  {
    is_group = true;
    name = name.substr(1);
  }
  if (!name.empty() && name[0] == '{')
  {
    elle::Buffer buf(name);
    elle::IOStream is(buf.istreambuf());
    auto key = elle::serialization::json::deserialize
      <infinit::cryptography::rsa::PublicKey>(is);
    return std::make_pair(key, is_group);
  }
  if (!is_group)
    return std::make_pair(ifnt.user_get(name).public_key, false);
  if (!mountpoint)
    throw elle::Error("A mountpoint is required to fetch groups.");
  char buf[32768];
  int res = port_getxattr(*mountpoint, "infinit.group.control_key." + name, buf, 16384, true);
  if (res <= 0)
    throw elle::Error("Unable to fetch group " + name);
  elle::Buffer b(buf, res);
  elle::IOStream is(b.istreambuf());
    auto key = elle::serialization::json::deserialize
      <infinit::cryptography::rsa::PublicKey>(is);
  return std::make_pair(key, is_group);
}

COMMAND(update)
{
  auto name = mandatory(args, "name", "network name");
  auto owner = self_user(ifnt, args);
  auto network = ifnt.network_get(name, owner);
  network.ensure_allowed(owner, "update");
  auto& dht = *network.dht();
  if (auto port = optional<int>(args, "port"))
    dht.port = port.get();
  if (compatibility_version)
    dht.version = compatibility_version.get();
  if (args.count("admin-r"))
  {
    for (auto u: args["admin-r"].as<std::vector<std::string>>())
    {
      auto r = user_key(u, optional(args, "mountpoint"));
      auto& target = r.second ? dht.admin_keys.group_r : dht.admin_keys.r;
      target.push_back(r.first);
    }
  }
  if (args.count("admin-rw"))
  {
    for (auto u: args["admin-rw"].as<std::vector<std::string>>())
    {
      auto r = user_key(u, optional(args, "mountpoint"));
      auto& target = r.second ? dht.admin_keys.group_w : dht.admin_keys.w;
      target.push_back(r.first);
    }
  }
  if (args.count("admin-remove"))
  {
    for (auto u: args["admin-remove"].as<std::vector<std::string>>())
    {
      auto r = user_key(u, optional(args, "mountpoint"));
#define DEL(cont) cont.erase(std::remove(cont.begin(), cont.end(), r.first), cont.end())
      DEL(dht.admin_keys.r);
      DEL(dht.admin_keys.w);
      DEL(dht.admin_keys.group_r);
      DEL(dht.admin_keys.group_w);
#undef DEL
    }
  }
  if (args.count("output"))
  {
    auto output = get_output(args);
    elle::serialization::json::serialize(network, *output, false);
  }
  else
  {
    ifnt.network_save(owner, network, true);
    report_updated("network", network.name);
  }
  if (aliased_flag(args, {"push-network", "push"}))
  {
    infinit::NetworkDescriptor desc(std::move(network));
    beyond_push("network", desc.name, desc, owner);
  }
}

COMMAND(export_)
{
  auto owner = self_user(ifnt, args);
  auto output = get_output(args);
  auto network_name = mandatory(args, "name", "network name");
  auto network = ifnt.network_get(network_name, owner);
  network_name = network.name;
  {
    infinit::NetworkDescriptor desc(std::move(network));
    elle::serialization::json::serialize(desc, *output, false);
  }
  report_exported(*output, "network", network_name);
}

COMMAND(fetch)
{
  auto self = self_user(ifnt, args);
  auto network_name_ = optional(args, "name");
  auto save = [&self] (infinit::NetworkDescriptor desc) {
    try
    {
      auto network = ifnt.network_get(desc.name, self, false);
      if (network.model)
      {
        auto* d = dynamic_cast<infinit::model::doughnut::Configuration*>(
          network.model.get()
        );
        infinit::Network updated_network(
          desc.name,
          elle::make_unique<infinit::model::doughnut::Configuration>(
            d->id,
            std::move(desc.consensus),
            std::move(desc.overlay),
            std::move(d->storage),
            self.keypair(),
            std::make_shared<infinit::cryptography::rsa::PublicKey>(desc.owner),
            d->passport,
            self.name,
            d->port,
            desc.version,
            desc.admin_keys));
        ifnt.network_save(self, updated_network, true);
      }
      else
      {
        // The network was present but not linked, just overwrite it with the
        // new descriptor.
        ifnt.network_save(desc, true);
      }
    }
    catch (MissingLocalResource const& e)
    {
      // The network wasn't present at all.
      ifnt.network_save(desc);
    }
  };
  if (network_name_)
  {
    std::string network_name = ifnt.qualified_name(network_name_.get(), self);
    save(beyond_fetch<infinit::NetworkDescriptor>("network", network_name));
  }
  else // Fetch all networks for self.
  {
    // FIXME: Workaround for NetworkDescriptor's copy constructor being deleted.
    // Remove when serialization does not require copy.
    auto res = beyond_fetch_json(elle::sprintf("users/%s/networks", self.name),
                                 "networks for user",
                                 self.name,
                                 self);
    auto root = boost::any_cast<elle::json::Object>(res);
    auto networks_vec =
      boost::any_cast<std::vector<elle::json::Json>>(root["networks"]);
    for (auto const& network_json: networks_vec)
    {
      try
      {
        elle::serialization::json::SerializerIn input(network_json, false);
        save(input.deserialize<infinit::NetworkDescriptor>());
      }
      catch (ResourceAlreadyFetched const& error)
      {}
    }
  }
}

COMMAND(import)
{
  auto input = get_input(args);
  auto desc =
    elle::serialization::json::deserialize<infinit::NetworkDescriptor>
    (*input, false);
  ifnt.network_save(desc);
  report_imported("network", desc.name);
}

COMMAND(link_)
{
  auto self = self_user(ifnt, args);
  auto network_name = mandatory(args, "name", "network name");
  auto storage = storage_configuration(args);
  auto desc = ifnt.network_descriptor_get(network_name, self);
  auto passport = [&] () -> infinit::Passport
  {
    if (self.public_key == desc.owner)
    {
      return infinit::Passport(
        self.public_key, desc.name,
        infinit::cryptography::rsa::KeyPair(self.public_key,
                                            self.private_key.get()));
    }
    try
    {
      return ifnt.passport_get(desc.name, self.name);
    }
    catch (MissingLocalResource const&)
    {
      throw elle::Error(
        elle::sprintf("missing passport (%s: %s), "
                      "use infinit-passport to fetch or import",
                      desc.name, self.name));
    }
  }();
  bool ok = passport.verify(
    passport.certifier() ? *passport.certifier() : desc.owner);
  if (!ok)
    throw elle::Error("passport signature is invalid");
  if (storage && !passport.allow_storage())
    throw elle::Error("passport does not allow storage");
  infinit::Network network(
    desc.name,
    elle::make_unique<infinit::model::doughnut::Configuration>(
      infinit::model::Address::random(0), // FIXME
      std::move(desc.consensus),
      std::move(desc.overlay),
      std::move(storage),
      self.keypair(),
      std::make_shared<infinit::cryptography::rsa::PublicKey>(desc.owner),
      std::move(passport),
      self.name,
      boost::optional<int>(),
      desc.version,
      desc.admin_keys));
  auto has_output = optional(args, "output");
  auto output = has_output ? get_output(args) : nullptr;
  if (output)
  {
    infinit::save(*output, network, false);
  }
  else
  {
    ifnt.network_save(self, network, true);
    report_action("linked", "device to network", network.name);
  }
}

COMMAND(list)
{
  auto self = self_user(ifnt, args);
  if (script_mode)
  {
    elle::json::Array l;
    for (auto const& network: ifnt.networks_get(self))
    {
      elle::json::Object o;
      o["name"] = network.name;
      o["linked"] = bool(network.model) && network.user_linked(self);
      l.push_back(std::move(o));
    }
    elle::json::write(std::cout, l);
  }
  else
  {
    for (auto const& network: ifnt.networks_get(self))
    {
      std::cout << network.name;
      if (network.model && network.user_linked(self))
        std::cout << ": linked";
      else
        std::cout << ": not linked";
      std::cout << std::endl;
    }
  }
}

COMMAND(unlink_)
{
  auto self = self_user(ifnt, args);
  auto network_name = mandatory(args, "name", "network name");
  auto network = ifnt.network_get(network_name, self, true);
  ifnt.network_unlink(network.name, self, true);
}

COMMAND(push)
{
  auto network_name = mandatory(args, "name", "network name");
  auto self = self_user(ifnt, args);
  auto network = ifnt.network_get(network_name, self);
  {
    auto& dht = *network.dht();
    auto owner_uid = infinit::User::uid(*dht.owner);
    infinit::NetworkDescriptor desc(std::move(network));
    beyond_push("network", desc.name, desc, self);
  }
}

COMMAND(pull)
{
  auto name_ = mandatory(args, "name", "network name");
  auto owner = self_user(ifnt, args);
  auto network_name = ifnt.qualified_name(name_, owner);
  beyond_delete("network", network_name, owner, false, flag(args, "purge"));
}


COMMAND(delete_)
{
  auto name = mandatory(args, "name", "network name");
  auto owner = self_user(ifnt, args);
  auto network = ifnt.network_get(name, owner, false);
  bool purge = flag(args, "purge");
  bool unlink = flag(args, "unlink");
  bool pull = flag(args, "pull");
  auto linked_users = ifnt.network_linked_users(network.name);
  if (linked_users.size() && !unlink)
  {
    std::vector<std::string> user_names;
    for (auto const& u: linked_users)
      user_names.emplace_back(u.name);
    throw elle::Error(
        elle::sprintf("Network is still linked with this device by %s. "
                      "Please unlink it first or add the --unlink flag",
                      user_names));
  }
  if (purge)
  {
    auto volumes = ifnt.volumes_for_network(network.name);
    std::vector<std::string> drives;
    for (auto const& volume: volumes)
    {
      auto vol_drives = ifnt.drives_for_volume(volume);
      drives.insert(drives.end(), vol_drives.begin(), vol_drives.end());
    }
    for (auto const& drive: drives)
    {
      auto drive_path = ifnt._drive_path(drive);
      if (boost::filesystem::remove(drive_path))
        report_action("deleted", "drive", drive, std::string("locally"));
    }
    for (auto const& volume: volumes)
    {
      auto vol_path = ifnt._volume_path(volume);
      if (boost::filesystem::remove(vol_path))
        report_action("deleted", "volume", volume, std::string("locally"));
    }
    for (auto const& user: ifnt.user_passports_for_network(network.name))
    {
      auto passport_path = ifnt._passport_path(network.name, user);
      if (boost::filesystem::remove(passport_path))
      {
        report_action("deleted", "passport",
                      elle::sprintf("%s: %s", network.name, user),
                      std::string("locally"));
      }
    }
  }
  if (pull)
    beyond_delete("network", network.name, owner, true, purge);
  ifnt.network_delete(name, owner, unlink, true);
}

COMMAND(run)
{
  auto name = mandatory(args, "name", "network name");
  auto self = self_user(ifnt, args);
  auto network = ifnt.network_get(name, self);
  network.ensure_allowed(self, "run");
  infinit::overlay::NodeEndpoints eps;
  if (args.count("peer"))
  {
    auto peers = args["peer"].as<std::vector<std::string>>();
    for (auto const& obj: peers)
    {
      auto file_eps = endpoints_from_file(obj);
      if (file_eps.size())
      {
        for (auto const& ep: file_eps)
          eps[infinit::model::Address::null].push_back(ep);
      }
      else
      {
        eps[infinit::model::Address::null].push_back(obj);
      }
    }
  }
  bool cache = flag(args, option_cache);
  auto cache_ram_size = optional<int>(args, option_cache_ram_size);
  auto cache_ram_ttl = optional<int>(args, option_cache_ram_ttl);
  auto cache_ram_invalidation =
    optional<int>(args, option_cache_ram_invalidation);
  auto disk_cache_size = optional<uint64_t>(args, option_cache_disk_size);
  if (cache_ram_size || cache_ram_ttl || cache_ram_invalidation
      || disk_cache_size)
  {
    cache = true;
  }
  auto port = optional<int>(args, option_port);
  auto dht = network.run(
    self,
    eps, false,
    cache, cache_ram_size, cache_ram_ttl, cache_ram_invalidation,
    flag(args, "async"), disk_cache_size, compatibility_version, port);
  // Only push if we have are contributing storage.
  bool push = aliased_flag(args, {"push-endpoints", "push", "publish"}) &&
    dht->local() && dht->local()->storage();
  bool fetch = aliased_flag(args, {"fetch-endpoints", "fetch", "publish"});
  if (!dht->local() && (!script_mode || push))
    elle::err("network %s is client only since no storage is attached", name);
  if (dht->local())
  {
    if (auto port_file = optional(args, option_port_file))
      port_to_file(dht->local()->server_endpoint().port(), port_file.get());
    if (auto endpoint_file = optional(args, option_endpoint_file))
      endpoints_to_file(dht->local()->server_endpoints(), endpoint_file.get());
  }
#ifndef INFINIT_WINDOWS
  if (flag(args, "daemon"))
    if (daemon(0, 1))
      perror("daemon:");
#endif
  auto run = [&]
    {
      if (fetch)
      {
        infinit::overlay::NodeEndpoints eps;
        beyond_fetch_endpoints(network, eps);
        dht->overlay()->discover(eps);
      }
      reactor::Thread::unique_ptr stat_thread;
      if (push)
        stat_thread = make_stat_update_thread(self, network, *dht);
      report_action("running", "network", network.name);
      if (script_mode)
      {
#ifndef INFINIT_WINDOWS
        reactor::FDStream stdin_stream(0);
#else
        // Windows does not support async io on stdin
        elle::Buffer input;
        while (true)
        {
          char buf[4096];
          std::cin.read(buf, 4096);
          int count = std::cin.gcount();
          if (count > 0)
            input.append(buf, count);
          else
          break;
        }
        auto stdin_stream = elle::IOStream(input.istreambuf());
#endif
        while (true)
        {
          try
          {
            auto json = boost::any_cast<elle::json::Object>(
              elle::json::read(stdin_stream));
            elle::serialization::json::SerializerIn command(json, false);
            command.set_context<infinit::model::doughnut::Doughnut*>(dht.get());
            auto op = command.deserialize<std::string>("operation");
            if (op == "fetch")
            {
              auto address =
                command.deserialize<infinit::model::Address>("address");
              auto block = dht->fetch(address);
              ELLE_ASSERT(block);
              elle::serialization::json::SerializerOut response(
                std::cout, false, true);
              response.serialize("success", true);
              response.serialize("value", block);
            }
            else if (op == "insert" || op == "update")
            {
              auto block = command.deserialize<
                std::unique_ptr<infinit::model::blocks::Block>>("value");
              if (!block)
                elle::err("missing field: value");
              dht->store(
                std::move(block),
                op == "insert" ?
                infinit::model::STORE_INSERT : infinit::model::STORE_UPDATE);
              elle::serialization::json::SerializerOut response(
                std::cout, false, true);
              response.serialize("success", true);
            }
            else
              elle::err("invalide operation: %s", op);
          }
          catch (elle::Error const& e)
          {
            if (stdin_stream.eof())
              return;
            elle::serialization::json::SerializerOut response(
              std::cout, false, true);
            response.serialize("success", false);
            response.serialize("message", e.what());
          }
        }
      }
      else
        reactor::sleep();
    };
  if (push)
  {
    elle::With<InterfacePublisher>(
      network, self, dht->overlay()->node_id(),
      dht->local()->server_endpoint().port()) << [&]
    {
      run();
    };
  }
  else
    run();
}

COMMAND(list_storage)
{
  auto owner = self_user(ifnt, args);
  auto network_name = mandatory(args, "name", "network name");
  auto network = ifnt.network_get(network_name, owner);
  if (network.model->storage)
  {
    if (auto strip = dynamic_cast<infinit::storage::StripStorageConfig*>(
        network.model->storage.get()))
    {
      for (auto const& s: strip->storage)
        std::cout << s->name << "\n";
    }
    else
    {
      std::cout << network.model->storage->name;
    }
    std::cout << std::endl;
  }
}

COMMAND(stats)
{
  auto owner = self_user(ifnt, args);
  std::string network_name = mandatory(args, "name", "network_name");
  std::string name = ifnt.qualified_name(network_name, owner);
  Storages res =
    beyond_fetch<Storages>(
      elle::sprintf("networks/%s/stat", name),
      "stat",
      "stat",
      boost::none,
      Headers{},
      false);

  // FIXME: write Storages::operator(std::ostream&)
  std::cout << "{\"usage\": " << res.usage
         << ", \"capacity\": " << res.capacity
         << "}" << std::endl;
}

int
main(int argc, char** argv)
{
  program = argv[0];
  using boost::program_options::value;
  using boost::program_options::bool_switch;
  Mode::OptionsDescription overlay_types_options("Overlay types");
  overlay_types_options.add_options()
    ("kelips", "use a Kelips overlay network (default)")
    ("kalimero", "use a Kalimero overlay network.\nUsed for local testing")
    ;
  Mode::OptionsDescription consensus_types_options("Consensus types");
  consensus_types_options.add_options()
    ("paxos", "use Paxos consensus algorithm (default)")
    ("no-consensus", "use no consensus algorithm")
    ;
  Mode::OptionsDescription kelips_options("Kelips options");
  kelips_options.add_options()
    ("nodes", value<int>(), "estimate of the total number of nodes")
    ("k", value<int>(), "number of groups (default: 1)")
    ("kelips-contact-timeout", value<std::string>(),
     "ping timeout before considering a peer lost (default: 2 min)")
    ("encrypt", value<std::string>(),
      "use encryption: no,lazy,yes (default: yes)")
    ("protocol", value<std::string>(),
      "RPC protocol to use: tcp,utp,all\n(default: all)")
    ;
  Modes modes {
    {
      "create",
      "Create a network",
      &create,
      "--name NAME "
        "[OVERLAY-TYPE OVERLAY-OPTIONS...] "
        "[CONSENSUS-TYPE CONSENSUS-OPTIONS...] "
        "[--storage STORAGE...]",
      {
        { "name,n", value<std::string>(), "created network name" },
        { "storage,S", value<std::vector<std::string>>()->multitoken(),
          "storage to contribute (optional, data striped over multiple)" },
        { "port", value<int>(), "port to listen on (default: random)" },
        { "replication-factor,r", value<int>(),
          "data replication factor (default: 1)" },
        { "eviction-delay,e", value<std::string>(),
          "missing servers eviction delay\n(default: 10 min)" },
        option_output("network"),
        { "push-network", bool_switch(),
          elle::sprintf("push the network to %s", beyond(true)) },
        { "push,p", bool_switch(), "alias for --push-network" },
        { "admin-r", value<std::vector<std::string>>()->multitoken(),
          "Set an admin user that can read all data"},
        { "admin-rw", value<std::vector<std::string>>()->multitoken(),
          "Set an admin user that can read and write all data"},
      },
      {
        consensus_types_options,
        overlay_types_options,
        kelips_options,
      },
    },
    {
      "update",
      "Update a network",
      &update,
      "--name NAME",
      {
        { "name,n", value<std::string>(), "network to update" },
        { "port", value<int>(), "port to listen on (default: random)" },
        option_output("network"),
        { "push-network", bool_switch(),
          elle::sprintf("push the updated network to %s", beyond(true)) },
        { "push,p", bool_switch(), "alias for --push-network" },
        { "admin-r", value<std::vector<std::string>>()->multitoken(),
          "Add an admin user that can read all data"},
        { "admin-rw", value<std::vector<std::string>>()->multitoken(),
          "Add an admin user that can read and write all data"},
        { "admin-remove", value<std::vector<std::string>>()->multitoken(),
          "Remove given user to all admin lists" },
        { "mountpoint", value<std::string>(),
          "Mountpoint of a volume using this network, "
          "required to add admin groups" },
      },
      {},
    },
    {
      "export",
      "Export a network",
      &export_,
      "--name NETWORK",
      {
        { "name,n", value<std::string>(), "network to export" },
        option_output("network"),
      },
    },
    {
      "fetch",
      elle::sprintf("Fetch a network from %s", beyond(true)),
      &fetch,
      {},
      {
        { "name,n", value<std::string>(), "network to fetch (optional)" },
      },
    },
    {
      "import",
      "Import a network",
      &import,
      {},
      {
        option_input("network"),
      },
    },
    {
      "link",
      "Link this device to a network",
      &link_,
      "--name NETWORK",
      {
        { "name,n", value<std::string>(), "network to link to" },
      },
      {},
      // Hidden options.
      {
        { "storage,S", value<std::vector<std::string>>()->multitoken(),
          "storage to contribute (optional)" },
        option_output("network"),
      },
    },
    {
      "unlink",
      "Unlink this device from a network",
      &unlink_,
      "--name NETWORK",
      {
        { "name,n", value<std::string>(), "network to unlink from" },
      },
      {},
      // Hidden options.
      {
        option_output("network"),
      },
    },
    {
      "list",
      "List networks",
      &list,
      {},
    },
    {
      "push",
      elle::sprintf("Push a network to %s", beyond(true)),
      &push,
      "--name NETWORK",
      {
        { "name,n", value<std::string>(), "network to push" },
      },
    },
    {
      "delete",
      "Delete a network locally",
      &delete_,
      "--name NETWORK",
      {
        { "name,n", value<std::string>(), "network to delete" },
        { "pull", bool_switch(),
          elle::sprintf("pull the network if it is on %s", beyond(true)) },
        { "purge", bool_switch(), "remove objects that depend on the network" },
        { "unlink", bool_switch(), "automatically unlink network if linked" },
      },
    },
    {
      "pull",
      elle::sprintf("Remove a network from %s", beyond(true)),
      &pull,
      "--name NETWORK",
      {
        { "name,n", value<std::string>(), "network to remove" },
        { "purge", bool_switch(), "remove objects that depend on the network" },
      },
    },
    {
      "run",
      "Run a network",
      &run,
      "--name NETWORK",
      {
        { "name,n", value<std::string>(), "network to run" },
        { "peer", value<std::vector<std::string>>()->multitoken(),
          "peer address or file with list of peer addresses (host:port)" },
        { "async", bool_switch(), "use asynchronous operations" },
        option_cache,
        option_cache_ram_size,
        option_cache_ram_ttl,
        option_cache_ram_invalidation,
        option_cache_disk_size,
        { "fetch-endpoints", bool_switch(),
          elle::sprintf("fetch endpoints from %s", beyond(true)) },
        { "fetch,f", bool_switch(), "alias for --fetch-endpoints" },
        { "push-endpoints", bool_switch(),
          elle::sprintf("push endpoints to %s", beyond(true)) },
        { "push,p", bool_switch(), "alias for --push-endpoints" },
        { "publish", bool_switch(),
          "alias for --fetch-endpoints --push-endpoints" },
        option_endpoint_file,
        option_port_file,
        option_port,
#ifndef INFINIT_WINDOWS
        { "daemon,d", bool_switch(), "run as a background daemon"},
#endif
      },
    },
    {
      "list-storage",
      "List all storage contributed by this device to a network",
      &list_storage,
      "--name NETWORK",
      {
        { "name,n", value<std::string>(), "network name" },
      },
    },
    {
      "stats",
      elle::sprintf("Fetch stats of a network on %s", beyond(true)),
      &stats,
      "--name NETWORK",
      {
        { "name,n", value<std::string>(), "network name" },
      },
    },
  };
  return infinit::main("Infinit network management utility", modes, argc, argv);
}
