#ifndef INFINIT_MODEL_DOUGHNUT_ACB_HH
# define INFINIT_MODEL_DOUGHNUT_ACB_HH

# include <elle/serialization/fwd.hh>

# include <cryptography/rsa/KeyPair.hh>

# include <infinit/model/User.hh>
# include <infinit/model/blocks/ACLBlock.hh>
# include <infinit/model/doughnut/OKB.hh>

namespace infinit
{
  namespace model
  {
    namespace doughnut
    {
      class ACB
        : public BaseOKB<blocks::ACLBlock>
      {
      /*------.
      | Types |
      `------*/
      public:
        typedef ACB Self;
        typedef BaseOKB<blocks::ACLBlock> Super;
        struct ACLEntry
        {
          infinit::cryptography::rsa::PublicKey key;
          bool read;
          bool write;
          elle::Buffer token;

          ACLEntry(infinit::cryptography::rsa::PublicKey key_,
           bool read_,
                   bool write_,
                   elle::Buffer token_);
          ACLEntry(ACLEntry const& other);
          ACLEntry(elle::serialization::SerializerIn& s);

          typedef infinit::serialization_tag serialization_tag;
          static ACLEntry deserialize(elle::serialization::SerializerIn& s);
        };

      /*-------------.
      | Construction |
      `-------------*/
      public:
        ACB(Doughnut* owner);
        ACB(ACB const& other);
        ELLE_ATTRIBUTE_R(int, editor);
        ELLE_ATTRIBUTE(elle::Buffer, owner_token);
        ELLE_ATTRIBUTE_R(Address, acl);
        ELLE_ATTRIBUTE(bool, acl_changed);
        ELLE_ATTRIBUTE(boost::optional<std::vector<ACLEntry>>, acl_entries);
        ELLE_ATTRIBUTE(int, data_version);
        ELLE_ATTRIBUTE(elle::Buffer, data_signature);
        ELLE_ATTRIBUTE(Address, prev_acl);

      /*-------.
      | Clone  |
      `-------*/
      public:
        virtual
        std::unique_ptr<blocks::Block>
        clone() const override;

      /*--------.
      | Content |
      `--------*/
      protected:
        virtual
        elle::Buffer
        _decrypt_data(elle::Buffer const& data) const;
        void _stored() override;
      /*------------.
      | Permissions |
      `------------*/
      public:
        virtual
        void
        set_permissions(cryptography::rsa::PublicKey const& key,
                        bool read,
                        bool write);
      protected:
        virtual
        void
        _set_permissions(model::User const& key,
                         bool read,
                         bool write) override;
        virtual
        void
        _copy_permissions(ACLBlock& to) override;
        virtual
        std::vector<Entry>
        _list_permissions() override;
        std::vector<ACLEntry>&
        acl_entries();
        std::vector<ACLEntry> const&
        acl_entries() const;

      /*-----------.
      | Validation |
      `-----------*/
      protected:
        virtual
        bool
        _compare_payload(BaseOKB<blocks::ACLBlock> const& other) const override;
        virtual
        blocks::ValidationResult
        _validate(blocks::Block const& previous) const override;
        virtual
        blocks::ValidationResult
        _validate() const override;
        virtual
        void
        _seal() override;
        virtual
        void
        _sign(elle::serialization::SerializerOut& s) const override;
      private:
        elle::Buffer
        _data_sign() const;

      /*--------------.
      | Serialization |
      `--------------*/
      public:
        ACB(elle::serialization::SerializerIn& input);
        virtual
        void
        serialize(elle::serialization::Serializer& s) override;
      private:
        void
        _serialize(elle::serialization::Serializer& input);
        std::unique_ptr<blocks::Block>
        _fetch_acl() const;
      };
    }
  }
}

#endif
