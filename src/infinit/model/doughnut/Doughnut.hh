#pragma once

#include <memory>

#include <boost/filesystem.hpp>
#include <boost/multi_index/hashed_index.hpp>
#include <boost/multi_index/identity.hpp>
#include <boost/multi_index/mem_fun.hpp>
#include <boost/multi_index/member.hpp>
#include <boost/multi_index/ordered_index.hpp>
#include <boost/multi_index/sequenced_index.hpp>
#include <boost/multi_index_container.hpp>

#include <elle/Defaulted.hh>
#include <elle/das/model.hh>
#include <elle/das/serializer.hh>
#include <elle/ProducerPool.hh>
#include <elle/cryptography/rsa/KeyPair.hh>

#include <infinit/model/Model.hh>
#include <infinit/model/doughnut/Consensus.hh>
#include <infinit/model/doughnut/Dock.hh>
#include <infinit/model/doughnut/Passport.hh>
#include <infinit/overlay/Overlay.hh>

namespace infinit
{
  namespace model
  {
    class MonitoringServer;

    namespace doughnut
    {
      namespace bmi = boost::multi_index;
      struct ACLEntry;
      ELLE_DAS_SYMBOL(w);
      ELLE_DAS_SYMBOL(r);
      ELLE_DAS_SYMBOL(group_r);
      ELLE_DAS_SYMBOL(group_w);

      struct AdminKeys
      {
        using PublicKeys = std::vector<elle::cryptography::rsa::PublicKey>;
        AdminKeys() = default;
        AdminKeys(AdminKeys&& b) = default;
        AdminKeys(AdminKeys const& b) = default;
        AdminKeys&
        operator = (AdminKeys&& b) = default;
        PublicKeys r;
        PublicKeys w;
        PublicKeys group_r;
        PublicKeys group_w;
        using Model =
          elle::das::Model<AdminKeys,
                     decltype(elle::meta::list(doughnut::r,
                                               doughnut::w,
                                               doughnut::group_r,
                                               doughnut::group_w))>;
      };

      ELLE_DAS_SYMBOL(admin_keys);
      ELLE_DAS_SYMBOL(connect_timeout);
      ELLE_DAS_SYMBOL(consensus_builder);
      ELLE_DAS_SYMBOL(id);
      ELLE_DAS_SYMBOL(keys);
      ELLE_DAS_SYMBOL(listen_address);
      ELLE_DAS_SYMBOL(monitoring_socket_path);
      ELLE_DAS_SYMBOL(name);
      ELLE_DAS_SYMBOL(overlay_builder);
      ELLE_DAS_SYMBOL(owner);
      ELLE_DAS_SYMBOL(passport);
      ELLE_DAS_SYMBOL(port);
      ELLE_DAS_SYMBOL(protocol);
      ELLE_DAS_SYMBOL(rdv_host);
      ELLE_DAS_SYMBOL(soft_fail_running);
      ELLE_DAS_SYMBOL(soft_fail_timeout);
      ELLE_DAS_SYMBOL(storage);
      ELLE_DAS_SYMBOL(version);

      class Doughnut // Doughnut. DougHnuT. Get it ?
        : public Model
        , public std::enable_shared_from_this<Doughnut>
      {
      /*-------------.
      | Construction |
      `-------------*/
      public:
        using OverlayBuilder = std::function<std::unique_ptr<infinit::overlay::Overlay> (Doughnut &, std::shared_ptr<Local>)>;
        using ConsensusBuilder = std::function<std::unique_ptr<consensus::Consensus> (Doughnut &)>;
        template <typename ... Args>
        Doughnut(Args&& ... args);
        ~Doughnut();
      private:
        using Init = decltype(
          elle::das::make_tuple(
            doughnut::id = std::declval<Address>(),
            doughnut::keys =
              std::declval<std::shared_ptr<elle::cryptography::rsa::KeyPair>>(),
            doughnut::owner =
              std::declval<std::shared_ptr<elle::cryptography::rsa::PublicKey>>(),
            doughnut::passport = std::declval<Passport>(),
            doughnut::consensus_builder = std::declval<ConsensusBuilder>(),
            doughnut::overlay_builder = std::declval<OverlayBuilder>(),
            doughnut::port = std::declval<boost::optional<int>>(),
            doughnut::listen_address =
              std::declval<boost::optional<boost::asio::ip::address>>(),
            doughnut::storage =
              std::declval<std::unique_ptr<storage::Storage>>(),
            doughnut::name = std::declval<boost::optional<std::string>>(),
            doughnut::version = std::declval<boost::optional<elle::Version>>(),
            doughnut::admin_keys = std::declval<AdminKeys>(),
            doughnut::rdv_host = std::declval<boost::optional<std::string>>(),
            doughnut::monitoring_socket_path =
              std::declval<boost::optional<boost::filesystem::path>>(),
            doughnut::protocol = std::declval<Protocol>(),
            doughnut::connect_timeout =
              std::declval<elle::Defaulted<std::chrono::milliseconds>>(),
            doughnut::soft_fail_timeout =
              std::declval<elle::Defaulted<std::chrono::milliseconds>>(),
            doughnut::soft_fail_running =
              std::declval<elle::Defaulted<bool>>()));
        Doughnut(Init init);
        ELLE_ATTRIBUTE_R(std::chrono::milliseconds, connect_timeout);
        ELLE_ATTRIBUTE_R(std::chrono::milliseconds, soft_fail_timeout);
        ELLE_ATTRIBUTE_R(bool, soft_fail_running);

      /*-----.
      | Time |
      `-----*/
      protected:
        virtual
        std::chrono::high_resolution_clock::time_point
        now();

      public:
        elle::cryptography::rsa::KeyPair const&
        keys() const;
        std::shared_ptr<elle::cryptography::rsa::KeyPair>
        keys_shared() const;
        bool
        verify(Passport const& passport,
               bool require_write,
               bool require_storage,
               bool require_sign);
        std::shared_ptr<elle::cryptography::rsa::PublicKey>
        resolve_key(uint64_t hash);
        int
        ensure_key(std::shared_ptr<elle::cryptography::rsa::PublicKey> const& k);
        ELLE_ATTRIBUTE_R(Address, id);
        ELLE_ATTRIBUTE(std::shared_ptr<elle::cryptography::rsa::KeyPair>, keys);
        ELLE_ATTRIBUTE_R(std::shared_ptr<elle::cryptography::rsa::PublicKey>, owner);
        ELLE_ATTRIBUTE_R(Passport, passport);
        ELLE_ATTRIBUTE_RX(AdminKeys, admin_keys);
        ELLE_ATTRIBUTE_R(std::unique_ptr<consensus::Consensus>, consensus)
        ELLE_ATTRIBUTE_R(std::shared_ptr<Local>, local)
        ELLE_ATTRIBUTE_RX(Dock, dock);
        ELLE_ATTRIBUTE_R(std::unique_ptr<overlay::Overlay>, overlay)
        ELLE_ATTRIBUTE(elle::reactor::Thread::unique_ptr, user_init)
        ELLE_ATTRIBUTE(
          elle::ProducerPool<std::unique_ptr<blocks::MutableBlock>>, pool)
        ELLE_ATTRIBUTE_RX(elle::reactor::Barrier, terminating);
        ELLE_ATTRIBUTE_r(Protocol, protocol);

      public:
        ELLE_ATTRIBUTE_R(KeyCache, key_cache);
      protected:

        std::unique_ptr<blocks::MutableBlock>
        _make_mutable_block() const override;

        std::unique_ptr<blocks::ImmutableBlock>
        _make_immutable_block(elle::Buffer content,
                              Address owner) const override;

        std::unique_ptr<blocks::ACLBlock>
        _make_acl_block() const override;

        std::unique_ptr<blocks::GroupBlock>
        _make_group_block() const override;

        std::unique_ptr<model::User>
        _make_user(elle::Buffer const& data) const override;
        std::unique_ptr<blocks::Block>
        _fetch(Address address,
               boost::optional<int> local_version) const override;

        void
        _fetch(std::vector<AddressVersion> const& addresses,
               std::function<void(Address, std::unique_ptr<blocks::Block>,
                 std::exception_ptr)> res) const override;

        void
        _insert(std::unique_ptr<blocks::Block> block,
                std::unique_ptr<ConflictResolver> resolver) override;
        void
        _update(std::unique_ptr<blocks::Block> block,
                std::unique_ptr<ConflictResolver> resolver) override;
        void
        _remove(Address address, blocks::RemoveSignature rs) override;
        friend class Local;
        ELLE_ATTRIBUTE(std::unique_ptr<MonitoringServer>, monitoring_server);

      /*------------------.
      | Service discovery |
      `------------------*/
      public:
        using Services = std::unordered_map<std::string, Address>;
        using ServicesTypes = std::unordered_map<std::string, Services>;
        ServicesTypes
        services();
        void
        service_add(std::string const& type,
                    std::string const& name,
                    elle::Buffer value);
        template <typename T>
        void
        service_add(std::string const& type,
                    std::string const&
                    name, T const& value);
      private:
        std::unique_ptr<blocks::MutableBlock>
        _services_block(bool write);

      /*----------.
      | Printable |
      `----------*/
      public:
        void
        print(std::ostream& out) const override;
      };

      struct Configuration
        : public ModelConfig
      {
      public:
        Address id;
        std::unique_ptr<consensus::Configuration> consensus;
        std::unique_ptr<overlay::Configuration> overlay;
        elle::cryptography::rsa::KeyPair keys;
        std::shared_ptr<elle::cryptography::rsa::PublicKey> owner;
        Passport passport;
        boost::optional<std::string> name;
        boost::optional<int> port;
        AdminKeys admin_keys;
        std::vector<Endpoints> peers;
        using Model = elle::das::Model<
          Configuration,
          decltype(elle::meta::list(symbols::overlay,
                                    symbols::keys,
                                    symbols::owner,
                                    symbols::passport,
                                    symbols::name))>;


        Configuration(
          Address id,
          std::unique_ptr<consensus::Configuration> consensus,
          std::unique_ptr<overlay::Configuration> overlay,
          std::unique_ptr<storage::StorageConfig> storage,
          elle::cryptography::rsa::KeyPair keys,
          std::shared_ptr<elle::cryptography::rsa::PublicKey> owner,
          Passport passport,
          boost::optional<std::string> name,
          boost::optional<int> port,
          elle::Version version,
          AdminKeys admin_keys,
          std::vector<Endpoints> peers);
        Configuration(Configuration&&) = default;
        Configuration(elle::serialization::SerializerIn& input);
        ~Configuration() override;
        void
        serialize(elle::serialization::Serializer& s) override;

        std::unique_ptr<infinit::model::Model>
        make(bool client,
             boost::filesystem::path const& p) override;
        std::unique_ptr<Doughnut>
        make(
          bool client,
          boost::filesystem::path const& p,
          bool async = false,
          bool cache = false,
          boost::optional<int> cach_size = {},
          boost::optional<std::chrono::seconds> cache_ttl = {},
          boost::optional<std::chrono::seconds> cache_invalidation = {},
          boost::optional<uint64_t> disk_cache_size = {},
          boost::optional<elle::Version> version = {},
          boost::optional<int> port = {},
          boost::optional<boost::asio::ip::address> listen_address = {},
          boost::optional<std::string> rdv_host = {},
          boost::optional<boost::filesystem::path> monitoring_socket_path = {});
      };

      std::string
      short_key_hash(elle::cryptography::rsa::PublicKey const& pub);
    }
  }
}

#include <infinit/model/doughnut/Doughnut.hxx>
