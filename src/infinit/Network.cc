#include <infinit/Network.hh>

#include <boost/filesystem.hpp>
#include <boost/range/algorithm/find_if.hpp>
#include <boost/range/algorithm_ext/erase.hpp>

#include <elle/format/hexadecimal.hh>

#include <elle/cryptography/hash.hh>

#include <elle/reactor/http/Request.hh>

#include <infinit/Infinit.hh>
#include <infinit/MountOptions.hh>
#include <infinit/model/Endpoints.hh>
#include <infinit/model/doughnut/Local.hh>
#include <infinit/utility.hh>

ELLE_LOG_COMPONENT("infinit.Network");

namespace bfs = boost::filesystem;

// FIXME: use model endpoints
struct Endpoints
{
  std::vector<std::string> addresses;
  int port;
  using Model = elle::das::Model<
    Endpoints,
    decltype(elle::meta::list(infinit::symbols::addresses,
                              infinit::symbols::port))>;
};
ELLE_DAS_SERIALIZE(Endpoints);

namespace infinit
{
  namespace
  {
    using Strings = std::vector<std::string>;

    /// Convert a list of peer names and/or file name of peer names,
    /// into a list of node locations.
    model::NodeLocations
    make_node_locations(boost::optional<Strings> peers)
    {
      auto res = model::NodeLocations{};
      if (peers)
        for (auto const& obj: *peers)
          if (bfs::exists(obj))
            for (auto const& peer: model::endpoints_from_file(obj))
              res.emplace_back(model::Address::null, model::Endpoints({peer}));
          else
            res.emplace_back(model::Address::null, model::Endpoints({obj}));
      return res;
    }
  }

  Network::Network(std::string name,
                   std::unique_ptr<model::ModelConfig> model,
                   boost::optional<std::string> description)
    : descriptor::TemplatedBaseDescriptor<Network>(std::move(name),
                                                   std::move(description))
    , model(std::move(model))
  {}

  Network::Network(elle::serialization::SerializerIn& s)
    : descriptor::TemplatedBaseDescriptor<Network>(s)
  {
    this->serialize(s);
  }

  void
  Network::serialize(elle::serialization::Serializer& s)
  {
    descriptor::TemplatedBaseDescriptor<Network>::serialize(s);
    s.serialize("model", this->model);
  }

  model::doughnut::Configuration*
  Network::dht() const
  {
    return static_cast<model::doughnut::Configuration*>(this->model.get());
  }

  bool
  Network::user_linked(infinit::User const& user) const
  {
    return this->model
      // Compare passport's public key and user public key.
      && this->dht()->passport.user() == user.public_key;
  }

  void
  Network::ensure_allowed(infinit::User const& user,
                          std::string const& action,
                          std::string const& resource) const
  {
    if (!this->user_linked(user))
      elle::err("You cannot %s this %s as %s",
                action, resource, user.name);
  }

  auto
  Network::run(User const& user,
               MountOptions const& mo,
               bool client,
               bool enable_monitoring,
               boost::optional<elle::Version> version,
               boost::optional<int> port)
    -> ThreadedOverlay
  {
    auto dht = this->run(
      user,
      client,
      mo.cache && *mo.cache,
      mo.cache_ram_size,
      mo.cache_ram_ttl,
      mo.cache_ram_invalidation,
      mo.async && *mo.async,
      mo.cache_disk_size,
      version,
      port,
      mo.listen_address
#ifndef INFINIT_WINDOWS
      , ((mo.enable_monitoring && *mo.enable_monitoring)
         || enable_monitoring)
#endif
      );
    auto eps = make_node_locations(mo.peers);
    auto poll_thread = [&] () -> elle::reactor::Thread::unique_ptr {
      if (mo.fetch.value_or(mo.publish.value_or(false)))
      {
        beyond_fetch_endpoints(eps);
        if (mo.poll_beyond && *mo.poll_beyond > 0)
         return
           this->make_poll_beyond_thread(*dht, eps, *mo.poll_beyond);
      }
      return {};
    }();
    dht->overlay()->discover(eps);
    return {std::move(dht), std::move(poll_thread)};
  }

  elle::reactor::Thread::unique_ptr
  Network::make_stat_update_thread(infinit::Infinit const& infinit,
                                   infinit::User const& self,
                                   infinit::model::doughnut::Doughnut& model)
  {
    auto notify = [&]
      {
        this->notify_storage(infinit, self, model.id());
      };
    model.local()->storage()->register_notifier(notify);
    return elle::reactor::every(60_min, "periodic storage stat updater", notify);
  }

  elle::reactor::Thread::unique_ptr
  Network::make_poll_beyond_thread(infinit::model::doughnut::Doughnut& model,
                                   infinit::overlay::NodeLocations const& locs_,
                                   int interval)
  {
    auto poll = [&, locs_, interval = boost::posix_time::seconds(interval)]
      {
        auto locs = locs_;
        while (true)
        {
          elle::reactor::sleep(interval);
          infinit::overlay::NodeLocations news;
          try
          {
            this->beyond_fetch_endpoints(news);
          }
          catch (elle::Error const& e)
          {
            ELLE_WARN("exception fetching endpoints: %s", e);
            continue;
          }
          auto new_addresses = std::unordered_set<infinit::model::Address>{};
          for (auto const& n: news)
          {
            auto const uid = n.id();
            new_addresses.insert(uid);
            auto it = boost::find_if(locs,
              [&](infinit::model::NodeLocation const& nl) { return nl.id() == uid;});
            if (it == locs.end())
            {
              ELLE_TRACE("calling discover() on new peer %s", n);
              locs.emplace_back(n);
              model.overlay()->discover({n});
            }
            else if (it->endpoints() != n.endpoints())
            {
              ELLE_TRACE("calling discover() on updated peer %s", n);
              it->endpoints() = n.endpoints();
              model.overlay()->discover({n});
            }
          }
          boost::remove_erase_if(locs,
            [&](infinit::model::NodeLocation const& nl)
            {
              return !contains(new_addresses, nl.id());
            });
        }
      };
    return elle::reactor::Thread::unique_ptr(new elle::reactor::Thread("beyond poller", poll));
  }

  std::unique_ptr<model::doughnut::Doughnut>
  Network::run(User const& user,
               bool client,
               bool cache,
               boost::optional<int> cache_size,
               boost::optional<int> cache_ttl,
               boost::optional<int> cache_invalidation,
               bool async_writes,
               boost::optional<uint64_t> disk_cache_size,
               boost::optional<elle::Version> version,
               boost::optional<int> port,
               boost::optional<boost::asio::ip::address> listen,
               bool enable_monitoring)
  {
    ELLE_LOG("client version: %s", infinit::version_describe());
    auto rdv_env = elle::os::getenv("INFINIT_RDV", "rdv.infinit.sh:7890");
    boost::optional<std::string> rdv_host;
    if (!rdv_env.empty())
      rdv_host = std::move(rdv_env);
    return this->dht()->make(
      client,
      this->cache_dir(user),
      async_writes,
      cache,
      cache_size,
      cache_ttl
      ? std::chrono::seconds(cache_ttl.get())
      : boost::optional<std::chrono::seconds>(),
      cache_invalidation
      ? std::chrono::seconds(cache_invalidation.get())
      : boost::optional<std::chrono::seconds>(),
      disk_cache_size,
      std::move(version),
      std::move(port),
      std::move(listen),
      std::move(rdv_host)
#ifndef INFINIT_WINDOWS
      , enable_monitoring ? this->monitoring_socket_path(user)
      : boost::optional<bfs::path>()
#endif
      );
  }

  void
  Network::notify_storage(infinit::Infinit const& infinit,
                          infinit::User const& user,
                          infinit::model::Address const& node_id)
  {
    ELLE_TRACE_SCOPE("push storage stats to %s", beyond());
    try
    {
      auto url = elle::sprintf(
        "networks/%s/stat/%s/%s", name, user.name, node_id);
      auto storage = this->dht()->storage->make();
      auto s = Storages{storage->usage(), storage->capacity()};
      infinit.beyond_push(
        url, "storage usage", name, std::move(s), user, false);
    }
    catch (elle::Error const& e)
    {
      ELLE_WARN("Error notifying storage size change: %s", e);
    }
  }

  bfs::path
  Network::cache_dir(User const& user) const
  {
    // "/cache" and "/async" are added by Doughnut.
    auto old_dir = xdg_state_home() / "cache" / this->name;
    auto new_dir = xdg_state_home() / "cache" / user.name / this->name;
    create_directories(new_dir / "async");
    if (bfs::exists(old_dir / "async") && !bfs::is_empty(old_dir / "async"))
    {
      for (auto p: bfs::recursive_directory_iterator(old_dir / "async"))
        if (is_visible_file(p))
          bfs::copy_file(p.path(),
                         new_dir / "async" / p.path().filename());
      bfs::remove_all(old_dir / "async");
    }
    if (bfs::exists(old_dir / "cache") && !bfs::is_empty(old_dir / "cache"))
    {
      ELLE_WARN("old cache location (\"%s/cache\") is being emptied in favor "
                "of the new cache location (\"%s/cache\")",
                old_dir.string(), new_dir.string());
      bfs::remove_all(old_dir / "cache");
    }
    return new_dir;
  }

  bfs::path
  Network::monitoring_socket_path(User const& user) const
  {
#ifdef INFINIT_WINDOWS
    elle::unreachable();
#else
    // UNIX-domain addresses are limited to 108 chars on Linux and 104 chars
    // on macOS ¯\_(ツ)_/¯
    namespace crypto = elle::cryptography;
    auto qualified_name = elle::sprintf(
      "%s/%s/%s", elle::system::username(), user.name, this->name);
    auto hashed = crypto::hash(qualified_name, crypto::Oneway::sha);
    auto socket_id = elle::format::hexadecimal::encode(hashed);
    return infinit::xdg_runtime_dir() / "monitor"
      / elle::sprintf("%s.sock", std::string(socket_id).substr(0, 6));
#endif
  }

  void
  Network::beyond_fetch_endpoints(infinit::model::NodeLocations& hosts,
                                  Reporter report)
  {
    auto url = elle::sprintf("%s/networks/%s/endpoints", beyond(), this->name);
    elle::reactor::http::Request r(url);
    elle::reactor::wait(r);
    if (r.status() != elle::reactor::http::StatusCode::OK)
      elle::err("unexpected HTTP error %s fetching endpoints for \"%s\"",
                r.status(), this->name);
    auto json = boost::any_cast<elle::json::Object>(elle::json::read(r));
    for (auto const& user: json)
    {
      try
      {
        for (auto const& node: boost::any_cast<elle::json::Object>(user.second))
        {
          auto uuid = infinit::model::Address::from_string(node.first);
          elle::serialization::json::SerializerIn s(node.second, false);
          auto endpoints = s.deserialize<Endpoints>();
          auto eps = infinit::model::Endpoints{};
          for (auto const& addr: endpoints.addresses)
            eps.emplace(boost::asio::ip::address::from_string(addr),
                        endpoints.port);
          hosts.emplace_back(uuid, std::move(eps));
        }
      }
      catch (std::exception const& e)
      {
        ELLE_WARN("Exception parsing peer endpoints: %s", e.what());
      }
    }
    if (report)
      report("fetched", "endpoints for", this->name);
  }

  void
  Network::print(std::ostream& out) const
  {
    out << "Network(" << this->name << ")";
  }

  NetworkDescriptor::NetworkDescriptor(
    std::string name,
    std::unique_ptr<model::doughnut::consensus::Configuration> consensus,
    std::unique_ptr<overlay::Configuration> overlay,
    elle::cryptography::rsa::PublicKey owner,
    elle::Version version,
    model::doughnut::AdminKeys admin_keys,
    std::vector<model::Endpoints> peers,
    boost::optional<std::string> description)
    : descriptor::TemplatedBaseDescriptor<NetworkDescriptor>(
      std::move(name), std::move(description))
    , consensus(std::move(consensus))
    , overlay(std::move(overlay))
    , owner(std::move(owner))
    , version(std::move(version))
    , admin_keys(std::move(admin_keys))
    , peers(std::move(peers))
  {}

  NetworkDescriptor::NetworkDescriptor(elle::serialization::SerializerIn& s)
    : descriptor::TemplatedBaseDescriptor<NetworkDescriptor>(s)
    , consensus(s.deserialize<std::unique_ptr<
                model::doughnut::consensus::Configuration>>("consensus"))
    , overlay(s.deserialize<std::unique_ptr<overlay::Configuration>>
              ("overlay"))
    , owner(s.deserialize<elle::cryptography::rsa::PublicKey>("owner"))
    , version()
  {
    try
    {
      version = s.deserialize<elle::Version>("version");
    }
    catch (elle::serialization::Error const&)
    {
      version = elle::Version(0, 3, 0);
    }
    try
    {
      s.serialize("admin_keys", this->admin_keys);
    }
    catch (elle::serialization::Error const&)
    {
    }
    try
    {
      s.serialize("peers", this->peers);
    }
    catch (elle::serialization::Error const&)
    {}
  }

  NetworkDescriptor::NetworkDescriptor(Network&& network)
    : descriptor::TemplatedBaseDescriptor<NetworkDescriptor>(
      std::move(network.name), std::move(network.description))
    , consensus(std::move(network.dht()->consensus))
    , overlay(std::move(network.dht()->overlay))
    , owner(std::move(*network.dht()->owner))
    , version(std::move(network.dht()->version))
    , admin_keys(std::move(network.dht()->admin_keys))
    , peers(std::move(network.dht()->peers))
  {}

  NetworkDescriptor::NetworkDescriptor(NetworkDescriptor const& desc)
    : descriptor::TemplatedBaseDescriptor<NetworkDescriptor>(desc)
    , consensus(desc.consensus->clone())
    , overlay(desc.overlay->clone())
    , owner(desc.owner)
    , version(desc.version)
    , admin_keys(desc.admin_keys)
    , peers(desc.peers)
  {}

  void
  NetworkDescriptor::serialize(elle::serialization::Serializer& s)
  {
    descriptor::TemplatedBaseDescriptor<NetworkDescriptor>::serialize(s);
    s.serialize("consensus", this->consensus);
    s.serialize("overlay", this->overlay);
    s.serialize("owner", this->owner);
    try
    {
      s.serialize("version", this->version);
    }
    catch (elle::serialization::Error const&)
    {
      // Oldest versions did not specify compatibility version.
      this->version = elle::Version(0, 3, 0);
    }
    try
    {
      s.serialize("admin_keys", this->admin_keys);
    }
    catch (elle::serialization::Error const&)
    {}
    try
    {
      s.serialize("peers", this->peers);
    }
    catch (elle::serialization::Error const&)
    {}
  }
}
