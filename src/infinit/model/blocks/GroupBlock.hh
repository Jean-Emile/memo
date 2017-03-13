#ifndef INFINIT_MODEL_BLOCKS_GROUP_BLOCK_HH
# define INFINIT_MODEL_BLOCKS_GROUP_BLOCK_HH

# include <elle/cryptography/rsa/KeyPair.hh>

# include <infinit/model/User.hh>
# include <infinit/model/blocks/ACLBlock.hh>

namespace infinit
{
  namespace model
  {
    namespace blocks
    {
      class GroupBlock
        : public ACLBlock
      {
      public:
        using Self = infinit::model::blocks::GroupBlock;
        using Super = infinit::model::blocks::ACLBlock;

        GroupBlock(GroupBlock const& other);
      protected:
        GroupBlock(Address);
        GroupBlock(Address, elle::Buffer data);
        friend class infinit::model::Model;

      public:
        virtual
        void
        add_member(model::User const& user);
        virtual
        void
        remove_member(model::User const& user);
        virtual
        void
        add_admin(model::User const& user);
        virtual
        void
        remove_admin(model::User const& user);
        virtual
        std::vector<std::unique_ptr<User>>
        list_admins(bool ommit_names) const;

      public:
        GroupBlock(elle::serialization::Serializer& input,
                   elle::Version const& version);
      };
    }
  }
}



#endif
