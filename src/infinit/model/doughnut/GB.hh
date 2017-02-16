#pragma once

#include <elle/serialization/fwd.hh>
#include <elle/Buffer.hh>

#include <cryptography/rsa/KeyPair.hh>

#include <infinit/model/User.hh>
#include <infinit/model/blocks/ACLBlock.hh>
#include <infinit/model/blocks/GroupBlock.hh>
#include <infinit/model/doughnut/OKB.hh>
#include <infinit/model/doughnut/ACB.hh>

namespace infinit
{
  namespace model
  {
    namespace doughnut
    {
      class GB
        : public BaseACB<blocks::GroupBlock>
      {
      /*-------------.
      | Construction |
      `-------------*/
      public:
        using Self = GB;
        using Super = BaseACB<blocks::GroupBlock>;
        GB(Doughnut* owner, cryptography::rsa::KeyPair master);
        GB(GB const& other);
        ~GB();

      public:
        void
        add_member(model::User const& user) override;
        void
        remove_member(model::User const& user) override;
        void
        add_admin(model::User const& user) override;
        void
        remove_admin(model::User const& user) override;
        std::vector<std::unique_ptr<model::User>>
        list_admins(bool ommit_names) const override;
        virtual
        cryptography::rsa::PublicKey
        current_public_key() const;
        virtual
        cryptography::rsa::KeyPair
        current_key() const;
        virtual
        std::vector<cryptography::rsa::KeyPair>
        all_keys() const;
        virtual
        std::vector<cryptography::rsa::PublicKey>
        all_public_keys() const;
        virtual
        int
        group_version() const;
        std::shared_ptr<cryptography::rsa::PrivateKey>
        control_key() const;
      protected:
        class OwnerSignature
          : public Super::OwnerSignature
        {
        public:
          OwnerSignature(GB const& block);
        protected:
          virtual
          void
          _serialize(elle::serialization::SerializerOut& s,
                     elle::Version const& v);
          ELLE_ATTRIBUTE_R(GB const&, block);
        };
        std::unique_ptr<typename BaseOKB<blocks::GroupBlock>::OwnerSignature>
        _sign() const override;
        class DataSignature
          : public Super::DataSignature
        {
        public:
          DataSignature(GB const& block);
          virtual
          void
          serialize(elle::serialization::Serializer& s_,
                    elle::Version const& v);
          ELLE_ATTRIBUTE_R(GB const&, block);
        };
        std::unique_ptr<Super::DataSignature>
        _data_sign() const override;

      /*--------------.
      | Serialization |
      `--------------*/
      public:
        GB(elle::serialization::SerializerIn& s,
           elle::Version const& version);
        void
        serialize(elle::serialization::Serializer& s,
                  elle::Version const& version) override;
      private:
        void
        _serialize(elle::serialization::Serializer& s,
                   elle::Version const& version);

      /*---------.
      | Clonable |
      `---------*/
      public:
        std::unique_ptr<blocks::Block>
        clone() const override;
      private:
        void
        _extract_keys();
        /// The decrypted group keys.
        ELLE_ATTRIBUTE(std::vector<infinit::cryptography::rsa::KeyPair>, keys);
        /// The group public keys, for other user to give the group access.
        ELLE_ATTRIBUTE_R(std::vector<infinit::cryptography::rsa::PublicKey>,
                         public_keys);
        // Order matter for signing, hence std::map.
        typedef std::map<infinit::cryptography::rsa::PublicKey, elle::Buffer>
          AdminKeys;
        /// The group admin keys ciphered for every admin.
        ELLE_ATTRIBUTE_R(AdminKeys, admin_keys);
      public:
        /// Optional group description.
        ELLE_ATTRIBUTE_rw(boost::optional<std::string>, description);

      public:
        typedef infinit::serialization_tag serialization_tag;

      /*----------.
      | Printable |
      `----------*/
      public:
        void
        print(std::ostream& ouptut) const override;
      };
    }
  }
}

