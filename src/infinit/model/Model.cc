#include <elle/log.hh>

#include <infinit/model/MissingBlock.hh>
#include <infinit/model/Model.hh>
#include <infinit/model/blocks/ACLBlock.hh>
#include <infinit/model/blocks/ImmutableBlock.hh>
#include <infinit/model/blocks/MutableBlock.hh>
#include <infinit/model/blocks/GroupBlock.hh>
#include <infinit/utility.hh>

ELLE_LOG_COMPONENT("infinit.model.Model");

namespace infinit
{
  namespace model
  {
    /*-------------.
    | Construction |
    `-------------*/

    Model::Model(Init args)
      : _version(args.version ? std::move(args.version.get()) :
                 elle::Version(infinit::version().major(),
                               infinit::version().minor(), 0))
      , make_immutable_block(
        elle::das::bind_method(*this, &Model::_make_immutable_block),
        data,
        owner = Address::null)
      , make_mutable_block(
        elle::das::bind_method(*this, &Model::_make_mutable_block))
      , fetch(
        elle::das::bind_method(*this, &Model::_fetch_impl),
        address,
        local_version = boost::optional<int>())
      , insert([this] (std::unique_ptr<blocks::Block> block,
                       std::unique_ptr<ConflictResolver> resolver)
               {
                 ELLE_TRACE_SCOPE("%s: insert %f", this, block);
                 block->seal();
                 this->_insert(std::move(block), std::move(resolver));
               },
               block,
               conflict_resolver = nullptr)
      , update([this] (std::unique_ptr<blocks::Block> block,
                       std::unique_ptr<ConflictResolver> resolver)
               {
                 ELLE_TRACE_SCOPE("%s: update %f", *this, *block);
                 block->seal();
                 return this->_update(std::move(block), std::move(resolver));
               },
               block,
               conflict_resolver = nullptr)
    {
      ELLE_LOG_COMPONENT("infinit.model.Model");
      ELLE_LOG("%s: compatibility version %s", this, this->_version);
      if (this->_version > infinit::version())
        elle::err(
          "compatibility version %s is too recent for infinit version %s",
          this->_version, infinit::version());
    }

    /*-------.
    | Blocks |
    `-------*/

    template <>
    std::unique_ptr<blocks::MutableBlock>
    Model::make_block(elle::Buffer data, Address addr) const
    {
      auto res = this->_make_mutable_block();
      res->data(std::move(data));
      return res;
    }

    std::unique_ptr<blocks::MutableBlock>
    Model::_make_mutable_block() const
    {
      ELLE_TRACE_SCOPE("%s: create block", *this);
      return this->_construct_block<blocks::MutableBlock>(
        Address::random(flags::mutable_block));
    }

    template <>
    std::unique_ptr<blocks::ImmutableBlock>
    Model::make_block(elle::Buffer data, Address owner) const
    {
      return this->_make_immutable_block(std::move(data), owner);
    }

    std::unique_ptr<blocks::ImmutableBlock>
    Model::_make_immutable_block(elle::Buffer data, Address owner) const
    {
      ELLE_TRACE_SCOPE("%s: create block", *this);
      return this->_construct_block<blocks::ImmutableBlock>(
        Address::random(flags::immutable_block), std::move(data));
    }

    template <>
    std::unique_ptr<blocks::ACLBlock>
    Model::make_block(elle::Buffer data, Address) const
    {
      auto res = this->_make_acl_block();
      res->data(std::move(data));
      return res;
    }

    template <>
    std::unique_ptr<blocks::GroupBlock>
    Model::make_block(elle::Buffer data, Address) const
    {
      auto res = this->_make_group_block();
      return res;
    }

    std::unique_ptr<blocks::ACLBlock>
    Model::_make_acl_block() const
    {
      ELLE_TRACE_SCOPE("%s: create ACL block", *this);
      return this->_construct_block<blocks::ACLBlock>(
        Address::random(flags::mutable_block));
    }

    std::unique_ptr<blocks::GroupBlock>
    Model::_make_group_block() const
    {
      return this->_construct_block<blocks::GroupBlock>(
        Address::random(flags::mutable_block));
    }

    std::unique_ptr<User>
    Model::make_user(elle::Buffer const& data) const
    {
      ELLE_TRACE_SCOPE("%s: load user from %f", *this, data);
      return this->_make_user(data);
    }

    std::unique_ptr<User>
    Model::_make_user(elle::Buffer const&) const
    {
      return std::make_unique<User>(); // FIXME
    }

    std::unique_ptr<blocks::Block>
    Model::_fetch_impl(Address address,
                       boost::optional<int> local_version) const
    {
      ELLE_TRACE_SCOPE("%s: fetch %f if newer than %s",
                       this, address, local_version);
      if (auto res = this->_fetch(address, local_version))
      {
        auto val = res->validate(*this, false);
        if (!val)
        {
          ELLE_WARN("%s: invalid block received for %s:%s", *this, address,
                    val.reason());
          elle::err("invalid block: %s", val.reason());
        }
        return res;
      }
      else
      {
        ELLE_ASSERT(local_version);
        return nullptr;
      }
    }

    void
    Model::multifetch(
      std::vector<AddressVersion> const& addresses,
      std::function<void(Address, std::unique_ptr<blocks::Block>,
                         std::exception_ptr)> res) const
    {
      ELLE_TRACE_SCOPE("%s: fetch %s blocks", this, addresses.size());
      this->_fetch(addresses, [&](Address addr,
                                  std::unique_ptr<blocks::Block> block,
                                  std::exception_ptr exception)
        {
          if (block && !block->validate(*this, false))
            res(addr, {},
                std::make_exception_ptr(elle::Error("invalid block")));
          else
            res(addr, std::move(block), exception);
        });
    }

    void
    Model::_fetch(std::vector<AddressVersion> const& addresses,
                  std::function<void(Address, std::unique_ptr<blocks::Block>,
                                     std::exception_ptr)> res) const
    {
      for (auto addr: addresses)
      {
        try
        {
          auto block = _fetch(addr.first, addr.second);
          res(addr.first, std::move(block), {});
        }
        catch (elle::Error const& e)
        {
          res(addr.first, {}, std::current_exception());
        }
      }
    }

    void
    Model::seal_and_insert(blocks::Block& block,
                           std::unique_ptr<ConflictResolver> resolver)
    {
      ELLE_TRACE_SCOPE("%s: insert %f", *this, block);
      block.seal();
      auto copy = block.clone();
      return this->_insert(std::move(copy), std::move(resolver));
    }

    void
    Model::seal_and_update(blocks::Block& block,
                           std::unique_ptr<ConflictResolver> resolver)
    {
      ELLE_TRACE_SCOPE("%s: update %f", *this, block);
      block.seal();
      auto copy = block.clone();
      return this->_update(std::move(copy), std::move(resolver));
    }

    void
    Model::remove(Address address)
    {
      ELLE_TRACE_SCOPE("%s: remove %f", this, address);
      auto block = this->fetch(address);
      auto rs = block->sign_remove(*this);
      this->remove(address, std::move(rs));
    }

    void
    Model::remove(Address address, blocks::RemoveSignature rs)
    {
      this->_remove(address, std::move(rs));
    }

    void
    Model::print(std::ostream& out) const
    {
      elle::fprintf(out, "%s(%x)",
                    elle::type_info(*this),
                    reinterpret_cast<void const*>(this));
    }

    ModelConfig::ModelConfig(std::unique_ptr<storage::StorageConfig> storage_,
                             elle::Version version_)
      : storage(std::move(storage_))
      , version(std::move(version_))
    {}

    ModelConfig::ModelConfig(elle::serialization::SerializerIn& s)
    {
      this->serialize(s);
    }

    void
    ModelConfig::serialize(elle::serialization::Serializer& s)
    {
      s.serialize("storage", this->storage);
      try
      {
        s.serialize("version", this->version);
      }
      catch (elle::Error)
      {
        // Oldest versions did not specify compatibility version.
        this->version = elle::Version(0, 3, 0);
      }
    }

    DummyConflictResolver::DummyConflictResolver()
    {}

    DummyConflictResolver::DummyConflictResolver(
      elle::serialization::SerializerIn& s,
      elle::Version const& version)
      : DummyConflictResolver()
    {
      this->serialize(s, version);
    }

    void
    DummyConflictResolver::serialize(elle::serialization::Serializer& s,
                                     elle::Version const& v)
    {}

    std::unique_ptr<blocks::Block>
    DummyConflictResolver::operator() (blocks::Block& block,
                                       blocks::Block& current)
    {
      ELLE_WARN("Conflict editing %f, dropping changes", block.address());
      if (auto mb = dynamic_cast<blocks::MutableBlock*>(&current))
      {
        auto v = mb->version();
        mb->seal(v + 1);
        ELLE_ASSERT_GT(mb->version(), v);
      }
      return current.clone();
    }

    SquashConflictResolverOptions::SquashConflictResolverOptions()
    : max_size(0)
    {}

    SquashConflictResolverOptions::SquashConflictResolverOptions(int max_size)
    : max_size(max_size)
    {}

    class MergeConflictResolver
      : public ConflictResolver
    {
    public:
      MergeConflictResolver()
      {}

      MergeConflictResolver(elle::serialization::SerializerIn& s,
                            elle::Version const& v)
      {
        serialize(s, v);
      }

      MergeConflictResolver(std::unique_ptr<ConflictResolver> a,
                            std::unique_ptr<ConflictResolver> b,
                            SquashConflictResolverOptions const& config)
      : _config(config)
      {
        this->_resolvers.push_back(std::move(a));
        this->_resolvers.push_back(std::move(b));
      }

      void
      add(std::unique_ptr<ConflictResolver> a)
      {
        this->_resolvers.push_back(std::move(a));
      }

      void
      add_front(std::unique_ptr<ConflictResolver> a)
      {
        std::vector<std::unique_ptr<ConflictResolver>> nr;
        nr.push_back(std::move(a));
        for (auto& cr: this->_resolvers)
          nr.push_back(std::move(cr));
        this->_resolvers = std::move(nr);
      }

      std::unique_ptr<blocks::Block>
      operator() (blocks::Block& block,
                  blocks::Block& current) override
      {
        auto res = (*this->_resolvers.front())(block, current);
        for (unsigned int i=1; i< _resolvers.size(); ++i)
          res = (*this->_resolvers[i])(block, *res);
        return res;
      }

      void
      serialize(elle::serialization::Serializer& s,
                elle::Version const& v) override
      {
        s.serialize("resolvers", this->_resolvers);
      }

      SquashOperation
      squashable(SquashStack const& b) override
      {
        elle::unreachable();
      }

      std::string
      description() const override
      {
        std::string res("Squash(");
        for (auto const& c: this->_resolvers)
          res += c->description() + ",";
        res += ')';
        return res;
      }

    private:
      SquashConflictResolverOptions _config;
      ELLE_ATTRIBUTE_RX(std::vector<std::unique_ptr<ConflictResolver>>,
                        resolvers);
    };

    std::vector<std::unique_ptr<ConflictResolver>>&
    get_merge_conflict_resolver_content(ConflictResolver& cr)
    {
      return dynamic_cast<MergeConflictResolver&>(cr).resolvers();
    }

    std::unique_ptr<ConflictResolver>
    make_merge_conflict_resolver(std::unique_ptr<ConflictResolver> a,
                                 std::unique_ptr<ConflictResolver> b,
                                 SquashConflictResolverOptions const& config)
    {
      if (auto m = dynamic_cast<MergeConflictResolver*>(a.get()))
      {
        m->add(std::move(b));
        return a;
      }
      else if (auto m = dynamic_cast<MergeConflictResolver*>(b.get()))
      {
        m->add_front(std::move(a));
        return b;
      }
      else
        return std::make_unique<MergeConflictResolver>(
          std::move(a), std::move(b), config);
    }

    static const elle::serialization::Hierarchy<model::ConflictResolver>::
    Register<MergeConflictResolver> _register_mcr("merge");

    SquashOperation
    ConflictResolver::squashable(ConflictResolver& prev)
    {
      // FIXME: check mcr max size
      if (auto mcr = dynamic_cast<MergeConflictResolver*>(&prev))
      {
        static const unsigned int max_size
          = std::stoi(elle::os::getenv("INFINIT_MAX_SQUASH_SIZE", "20"));
        if (mcr->resolvers().size() >= max_size)
          return {model::Squash::stop, {}};
        return this->squashable(mcr->resolvers());
      }
      else
      {
        SquashStack stack;
        stack.emplace_back(&prev);
        elle::SafeFinally releaser([&] { stack.front().release();});
        return this->squashable(stack);
      }
    }

    std::string
    DummyConflictResolver::description() const
    {
      return "unknown";
    }

    static const elle::serialization::Hierarchy<ConflictResolver>::
    Register<DummyConflictResolver> _register_dcr("dummy");
  }
}
