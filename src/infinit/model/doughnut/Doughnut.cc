#include <infinit/model/doughnut/Doughnut.hh>

#include <boost/optional.hpp>

#include <elle/Buffer.hh>
#include <elle/Error.hh>
#include <elle/IOStream.hh>
#include <elle/cast.hh>
#include <elle/format/hexadecimal.hh>
#include <elle/log.hh>
#include <elle/serialization/json.hh>

#include <cryptography/hash.hh>

#include <reactor/Scope.hh>
#include <reactor/exception.hh>
#include <reactor/network/utp-server.hh>

#include <infinit/model/MissingBlock.hh>
#include <infinit/model/blocks/ImmutableBlock.hh>
#include <infinit/model/blocks/MutableBlock.hh>
#include <infinit/model/doughnut/ACB.hh>
#include <infinit/model/doughnut/CHB.hh>
#include <infinit/model/doughnut/GB.hh>
#include <infinit/model/doughnut/Local.hh>
#include <infinit/model/doughnut/OKB.hh>
#include <infinit/model/doughnut/Remote.hh>
#include <infinit/model/doughnut/UB.hh>
#include <infinit/model/doughnut/User.hh>
#include <infinit/model/doughnut/Group.hh>
#include <infinit/model/doughnut/Consensus.hh>
#include <infinit/model/doughnut/Async.hh>
#include <infinit/model/doughnut/Cache.hh>
#include <infinit/model/doughnut/consensus/Paxos.hh>
#include <infinit/model/doughnut/conflict/UBUpserter.hh>
#include <infinit/storage/MissingKey.hh>

ELLE_LOG_COMPONENT("infinit.model.doughnut.Doughnut");

namespace infinit
{
  namespace model
  {
    namespace doughnut
    {
      /*-------------.
      | Construction |
      `-------------*/

      Doughnut::Doughnut(Address id,
                         std::shared_ptr<cryptography::rsa::KeyPair> keys,
                         std::shared_ptr<cryptography::rsa::PublicKey> owner,
                         Passport passport,
                         ConsensusBuilder consensus,
                         OverlayBuilder overlay_builder,
                         boost::optional<int> port,
                         boost::optional<boost::asio::ip::address> listen_address,
                         std::unique_ptr<storage::Storage> storage,
                         boost::optional<elle::Version> version,
                         AdminKeys const& admin_keys,
                         boost::optional<std::string> rdv_host,
                         Protocol protocol)
        : Model(std::move(version))
        , _id(std::move(id))
        , _keys(keys)
        , _owner(std::move(owner))
        , _passport(std::move(passport))
        , _admin_keys(admin_keys)
        , _consensus(consensus(*this))
        , _local(
          storage ?
          this->_consensus->make_local(
            port, listen_address, std::move(storage), protocol) :
          nullptr)
        // FIXME: move protocol configuration to doughnut
        , _dock(*this, Protocol::all, port, listen_address, std::move(rdv_host))
        , _overlay(overlay_builder(*this, this->_local))
        , _pool([this] { return elle::make_unique<ACB>(this); }, 100, 1)
        , _terminating()
      {
        if (this->_local)
          this->_local->initialize();
      }

      template<typename ConflictResolver, typename F, typename FF, typename... Args>
      void check_push(Doughnut& d,
                      std::string const& what,
                      Address where,
                      F (UB::*checker)(void) const,
                      FF value,
                      Args... create)
      {
        while (true)
          try
          {
            ELLE_TRACE_SCOPE("%s: check %s", d, what);
            auto block = d.fetch(where);
            ELLE_DEBUG("%s: %s already present at %x",
                       d, what, block->address());
            auto ub = elle::cast<UB>::runtime(block);
            if (((ub.get())->*checker)() != value)
              elle::err("%s: %s exists but differrs at %x: %s != %s",
                        d, what, where, value, ((ub.get())->*checker)());
            break;
          }
          catch (MissingBlock const&)
          {
            auto user = elle::make_unique<UB>(create...);
            ELLE_TRACE_SCOPE("%s: store %s at %f",
              d, what, user->address());
            try
            {
              d.store(
                std::move(user), STORE_INSERT,
                elle::make_unique<ConflictResolver>(what));
            }
            catch (elle::Error const& e)
            {
              ELLE_TRACE("%s: failed to store %s: %s", d, what, e);
              if (d.terminating().opened())
                break;
              reactor::wait(d.terminating(), 1_sec);
            }
          }
      }

      Doughnut::Doughnut(Address id,
                         std::string const& name,
                         std::shared_ptr<cryptography::rsa::KeyPair> keys,
                         std::shared_ptr<cryptography::rsa::PublicKey> owner,
                         Passport passport,
                         ConsensusBuilder consensus,
                         OverlayBuilder overlay_builder,
                         boost::optional<int> port,
                         boost::optional<boost::asio::ip::address> listen_address,
                         std::unique_ptr<storage::Storage> storage,
                         boost::optional<elle::Version> version,
                         AdminKeys const& admin_keys,
                         boost::optional<std::string> rdv_host,
                         Protocol protocol)
        : Doughnut(std::move(id),
                   std::move(keys),
                   std::move(owner),
                   std::move(passport),
                   std::move(consensus),
                   std::move(overlay_builder),
                   std::move(port),
                   std::move(listen_address),
                   std::move(storage),
                   std::move(version),
                   admin_keys,
                   std::move(rdv_host),
                   protocol)
      {
        auto check_user_blocks = [name, this]
          {
            check_push<UserBlockUpserter>(*this,
              elle::sprintf("user block for %s", name),
              UB::hash_address(name, *this),
              &UB::key,
              this->keys().K(),
              this, name, this->passport());
            check_push<ReverseUserBlockUpserter>(*this,
              elle::sprintf("reverse user block for %s", name),
              UB::hash_address(this->keys().K(), *this),
              &UB::name,
              name,
              this, name, this->passport(), true);
            auto hash = UB::hash(this->keys().K());
            check_push<UserBlockUpserter>(*this,
              elle::sprintf("key hash block for %s", name),
              UB::hash_address(':' + hash.string(), *this),
              &UB::key,
              this->keys().K(),
              this, ':' + hash.string(), this->keys().K());
          };
        this->_user_init.reset(new reactor::Thread(
          elle::sprintf("%s: user blocks checker", *this),
          check_user_blocks));
      }

      Doughnut::~Doughnut()
      {
        ELLE_TRACE_SCOPE("%s: destruct", this);
        this->_terminating.open();
        if (this->_user_init)
        {
          if (!reactor::wait(*this->_user_init, 5_sec))
            this->_user_init->terminate_now();
          this->_user_init.reset();
        }
        if (this->_local)
          this->_local->cleanup();
        this->_consensus.reset();
        this->_dock.cleanup();
        this->_overlay.reset();
        this->_dock.cleanup();
        if (this->_local)
        {
          if (!this->_local.unique())
          {
            ELLE_ABORT("Doughnut destroyed with %s extra references to Local",
                       this->_local.use_count() - 1);
          }
          this->_local.reset();
        }
      }

      /*-----.
      | Time |
      `-----*/

      std::chrono::high_resolution_clock::time_point
      Doughnut::now()
      {
        return std::chrono::high_resolution_clock::now();
      }

      cryptography::rsa::KeyPair const&
      Doughnut::keys() const
      {
        return *this->_keys;
      }

      std::shared_ptr<cryptography::rsa::KeyPair>
      Doughnut::keys_shared() const
      {
        return this->_keys;
      }

      std::unique_ptr<blocks::MutableBlock>
      Doughnut::_make_mutable_block() const
      {
        ELLE_TRACE_SCOPE("%s: create OKB", *this);
        return elle::make_unique<OKB>(elle::unconst(this));
      }

      std::unique_ptr<blocks::ImmutableBlock>
      Doughnut::_make_immutable_block(elle::Buffer content, Address owner) const
      {
        ELLE_TRACE_SCOPE("%s: create CHB", *this);
        return elle::make_unique<CHB>(elle::unconst(this),
                                      std::move(content), owner);
      }

      std::unique_ptr<blocks::ACLBlock>
      Doughnut::_make_acl_block() const
      {
        ELLE_TRACE_SCOPE("%s: create ACB", *this);
        return elle::cast<blocks::ACLBlock>::runtime(
          elle::unconst(this)->_pool.get());
      }

      std::unique_ptr<blocks::GroupBlock>
      Doughnut::_make_group_block() const
      {
        return elle::make_unique<GB>(
          elle::unconst(this),
          cryptography::rsa::keypair::generate(2048));
      }

      std::unique_ptr<model::User>
      Doughnut::_make_user(elle::Buffer const& data) const
      {
        if (data.size() == 0)
          throw elle::Error("invalid empty user");
        if (data[0] == '{')
        {
          ELLE_TRACE_SCOPE("%s: fetch user from public key", *this);
          elle::IOStream input(data.istreambuf());
          elle::serialization::json::SerializerIn s(input);
          cryptography::rsa::PublicKey pub(s);
          try
          {
            auto block = this->fetch(UB::hash_address(pub, *this));
            auto ub = elle::cast<UB>::runtime(block);
            return elle::make_unique<doughnut::User>(ub->key(), ub->name());
          }
          catch (MissingBlock const&)
          {
            ELLE_TRACE("Reverse UB not found, returning public key hash");
            auto hash = short_key_hash(pub);
            return elle::make_unique<doughnut::User>(pub, hash);
          }
        }
        else if (data[0] == '@')
        {
          ELLE_TRACE_SCOPE("%s: fetch user from group", *this);
          auto gn = data.string().substr(1);
          Group g(*elle::unconst(this), gn);
          return elle::make_unique<doughnut::User>(g.public_control_key(),
                                                   data.string());
        }
        else
        {
          ELLE_TRACE_SCOPE("%s: fetch user from name", *this);
          try
          {
            auto block = this->fetch(UB::hash_address(data.string(), *this));
            auto ub = elle::cast<UB>::runtime(block);
            return elle::make_unique<doughnut::User>(ub->key(), data.string());
          }
          catch (infinit::model::MissingBlock const&)
          {
            return nullptr;
          }
        }
      }

      void
      Doughnut::_store(std::unique_ptr<blocks::Block> block,
                       StoreMode mode,
                       std::unique_ptr<ConflictResolver> resolver)
      {
        this->_consensus->store(std::move(block),
                                mode,
                                std::move(resolver));
      }

      std::unique_ptr<blocks::Block>
      Doughnut::_fetch(Address address,
                       boost::optional<int> local_version) const
      {
        return this->_consensus->fetch(address, std::move(local_version));
      }

      void
      Doughnut::_fetch(std::vector<AddressVersion> const& addresses,
        std::function<void(Address, std::unique_ptr<blocks::Block>,
          std::exception_ptr)> res) const
      {
        this->_consensus->fetch(addresses, res);
      }

      void
      Doughnut::_remove(Address address, blocks::RemoveSignature rs)
      {
        this->_consensus->remove(address, std::move(rs));
      }

      bool
      Doughnut::verify(Passport const& passport,
                         bool require_write,
                         bool require_storage,
                         bool require_sign)
      {
        ELLE_TRACE_SCOPE("%s: validating passport %s", this, passport);
        if (  (require_write && !passport.allow_write())
           || (require_storage && !passport.allow_storage())
           || (require_sign && !passport.allow_sign())
         )
        {
          ELLE_TRACE("%s: passport permissions mismatch", *this);
          return false;
        }
        if (!passport.certifier() || *passport.certifier() == *this->owner())
        {
          ELLE_TRACE("%s: validating with owner key", *this);
          return passport.verify(*this->owner());
        }
        if (!passport.verify(*passport.certifier()))
        {
          ELLE_TRACE("%s: validating with certifier key %x", *this,
                     *passport.certifier());
          return false;
        }
        // fetch passport for certifier
        try
        {
          auto const addr = UB::hash_address(*passport.certifier(), *this);
          auto block = this->fetch(addr);
          auto ub = elle::cast<UB>::runtime(block);
          if (!ub->passport())
          {
            ELLE_TRACE("%s: certifier RUB does not contain a passport", *this);
            return false;
          }
          return verify(*ub->passport(), false, false, true);
        }
        catch (elle::Exception const& e)
        {
          ELLE_TRACE("%s: exception fetching/validating: %s",
                     *this, e);
          return false;
        }
      }

      int
      Doughnut::ensure_key(std::shared_ptr<cryptography::rsa::PublicKey> const& k)
      {
        auto it = this->_key_cache.get<0>().find(*k);
        if (it != this->_key_cache.get<0>().end())
          return it->hash;

        int index = this->_key_cache.get<0>().size();
        this->_key_cache.insert(KeyHash(index, k));
        return index;
      }

      std::shared_ptr<cryptography::rsa::PublicKey>
      Doughnut::resolve_key(uint64_t hash)
      {
        ELLE_DUMP("%s: resolve key from %x", this, hash);
        auto it = this->_key_cache.get<1>().find(hash);
        if (it != this->_key_cache.get<1>().end())
        {
          ELLE_DUMP("%s: resolved from cache: %x", this, hash);
          return it->key;
        }
        elle::err("%s: failed to resolve key hash locally: %x", this, hash);
      }

      Configuration::~Configuration()
      {}

      Configuration::Configuration(
        Address id_,
        std::unique_ptr<consensus::Configuration> consensus_,
        std::unique_ptr<overlay::Configuration> overlay_,
        std::unique_ptr<storage::StorageConfig> storage,
        cryptography::rsa::KeyPair keys_,
        std::shared_ptr<cryptography::rsa::PublicKey> owner_,
        Passport passport_,
        boost::optional<std::string> name_,
        boost::optional<int> port_,
        elle::Version version,
        AdminKeys admin_keys)
        : ModelConfig(std::move(storage), std::move(version))
        , id(std::move(id_))
        , consensus(std::move(consensus_))
        , overlay(std::move(overlay_))
        , keys(std::move(keys_))
        , owner(std::move(owner_))
        , passport(std::move(passport_))
        , name(std::move(name_))
        , port(std::move(port_))
        , admin_keys(std::move(admin_keys))
      {}

      Configuration::Configuration(elle::serialization::SerializerIn& s)
        : ModelConfig(s)
        , id(s.deserialize<Address>("id"))
        , consensus(s.deserialize<std::unique_ptr<consensus::Configuration>>(
                      "consensus"))
        , overlay(s.deserialize<std::unique_ptr<overlay::Configuration>>(
                    "overlay"))
        , keys(s.deserialize<cryptography::rsa::KeyPair>("keys"))
        , owner(
          s.deserialize<std::shared_ptr<cryptography::rsa::PublicKey>>("owner"))
        , passport(s.deserialize<Passport>("passport"))
        , name(s.deserialize<boost::optional<std::string>>("name"))
        , port(s.deserialize<boost::optional<int>>("port"))
      {
        try
        {
          s.serialize("admin_keys", this->admin_keys);
        }
        catch (elle::serialization::Error const&)
        {
        }
      }

      void
      Configuration::serialize(elle::serialization::Serializer& s)
      {
        ModelConfig::serialize(s);
        s.serialize("id", this->id);
        s.serialize("consensus", this->consensus);
        s.serialize("overlay", this->overlay);
        s.serialize("keys", this->keys);
        s.serialize("owner", this->owner);
        s.serialize("passport", this->passport);
        s.serialize("name", this->name);
        s.serialize("port", this->port);
        s.serialize("admin_keys", this->admin_keys);
      }

      std::unique_ptr<infinit::model::Model>
      Configuration::make(bool client,
                          boost::filesystem::path const& dir)
      {
        return this->make(client, dir, false, false);
      }

      std::unique_ptr<Doughnut>
      Configuration::make(
        bool client,
        boost::filesystem::path const& p,
        bool async,
        bool cache,
        boost::optional<int> cache_size,
        boost::optional<std::chrono::seconds> cache_ttl,
        boost::optional<std::chrono::seconds> cache_invalidation,
        boost::optional<uint64_t> disk_cache_size,
        boost::optional<elle::Version> version,
        boost::optional<int> port_,
        boost::optional<boost::asio::ip::address> listen_address,
        boost::optional<std::string> rdv_host)
      {
        Doughnut::ConsensusBuilder consensus =
          [&] (Doughnut& dht)
          {
            if (!this->consensus)
            {
              elle::err(
                "invalid network configuration, missing field \"consensus\"");
            }
            auto consensus = this->consensus->make(dht);
            if (async)
            {
              consensus = elle::make_unique<consensus::Async>(
                std::move(consensus), p / "async");
            }
            if (cache)
            {
              consensus = elle::make_unique<consensus::Cache>(
                std::move(consensus),
                std::move(cache_size),
                std::move(cache_invalidation),
                std::move(cache_ttl),
                p / "cache",
                std::move(disk_cache_size)
                );
            }
            return consensus;
          };
        Doughnut::OverlayBuilder overlay =
          [&] (Doughnut& dht, std::shared_ptr<Local> local)
          {
            if (!this->overlay)
              elle::err(
                "invalid network configuration, missing field \"overlay\"");
            return this->overlay->make(std::move(local), &dht);
          };
        auto port = port_ ? port_.get() : this->port ? this->port.get() : 0;
        std::unique_ptr<storage::Storage> storage;
        if (this->storage)
          storage = this->storage->make();
        std::unique_ptr<Doughnut> dht;
        if (!client || !this->name)
        {
          dht = elle::make_unique<infinit::model::doughnut::Doughnut>(
            this->id,
            std::make_shared<cryptography::rsa::KeyPair>(keys),
            owner,
            passport,
            std::move(consensus),
            std::move(overlay),
            std::move(port),
            std::move(listen_address),
            std::move(storage),
            version ? version.get() : this->version,
            admin_keys,
            std::move(rdv_host));
        }
        else
        {
          dht = elle::make_unique<infinit::model::doughnut::Doughnut>(
            this->id,
            this->name.get(),
            std::make_shared<cryptography::rsa::KeyPair>(keys),
            owner,
            passport,
            std::move(consensus),
            std::move(overlay),
            std::move(port),
            std::move(listen_address),
            std::move(storage),
            version ? version.get() : this->version,
            admin_keys,
            std::move(rdv_host));
        }
        return dht;
      }

      std::string
      short_key_hash(cryptography::rsa::PublicKey const& pub)
      {
        auto key_hash = UB::hash(pub);
        std::string hex_hash = elle::format::hexadecimal::encode(key_hash);
        return elle::sprintf("#%s", hex_hash.substr(0, 6));
      }

      static const elle::serialization::Hierarchy<ModelConfig>::
      Register<Configuration> _register_Configuration("doughnut");
      static const elle::TypeInfo::RegisterAbbrevation
      _dht_abbr("infinit::model::doughnut", "dht");
    }
  }
}
