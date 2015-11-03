#ifndef INFINIT_MODEL_MODEL_HH
# define INFINIT_MODEL_MODEL_HH

# include <memory>
# include <boost/filesystem.hpp>
# include <elle/UUID.hh>

# include <infinit/model/Address.hh>
# include <infinit/model/User.hh>
# include <infinit/model/blocks/fwd.hh>
# include <infinit/serialization.hh>

namespace infinit
{
  namespace overlay
  {
    typedef std::unordered_map<elle::UUID, std::vector<std::string>> NodeEndpoints;
  }
  namespace model
  {
    enum StoreMode
    {
      STORE_ANY,
      STORE_INSERT,
      STORE_UPDATE
    };

    // Called in case of conflict error. Returns the new block to retry with
    // or null to abort
    class ConflictResolver
      : public elle::serialization::VirtuallySerializable
    {
    public:
      virtual std::unique_ptr<blocks::Block>
      operator () (blocks::Block& block, StoreMode mode) = 0;
      virtual void serialize(elle::serialization::Serializer& s) override = 0;
    };

    class Model
    {
    public:
      Model();
      template <typename Block>
      std::unique_ptr<Block>
      make_block(elle::Buffer data = elle::Buffer()) const;
      std::unique_ptr<User>
      make_user(elle::Buffer const& data) const;
      void
      store(std::unique_ptr<blocks::Block> block,
            StoreMode mode = STORE_ANY,
            std::unique_ptr<ConflictResolver> = {});
      void
      store(blocks::Block& block,
            StoreMode mode = STORE_ANY,
            std::unique_ptr<ConflictResolver> = {}); 
      std::unique_ptr<blocks::Block>
      fetch(Address address) const;
      void
      remove(Address address);
    protected:
      template <typename Block, typename ... Args>
      static
      std::unique_ptr<Block>
      _construct_block(Args&& ... args);
      virtual
      std::unique_ptr<blocks::MutableBlock>
      _make_mutable_block() const;
      virtual
      std::unique_ptr<blocks::ImmutableBlock>
      _make_immutable_block(elle::Buffer content) const;
      virtual
      std::unique_ptr<blocks::ACLBlock>
      _make_acl_block() const;
      virtual
      std::unique_ptr<User>
      _make_user(elle::Buffer const& data) const;
      virtual
      void
      _store(std::unique_ptr<blocks::Block> block,
             StoreMode mode,
             std::unique_ptr<ConflictResolver> resolver) = 0;
      virtual
      std::unique_ptr<blocks::Block>
      _fetch(Address address) const = 0;
      virtual
      void
      _remove(Address address) = 0;
    };

    struct ModelConfig:
      public elle::serialization::VirtuallySerializable
    {
      static constexpr char const* virtually_serializable_key = "type";

      virtual
      std::unique_ptr<infinit::model::Model>
      make(overlay::NodeEndpoints const& hosts, bool client, bool server,
           boost::filesystem::path const& dir) = 0;
      typedef infinit::serialization_tag serialization_tag;
    };
  }
}

# include <infinit/model/Model.hxx>

#endif
