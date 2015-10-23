#ifndef INFINIT_MODEL_DOUGHNUT_OKB_HH
# define INFINIT_MODEL_DOUGHNUT_OKB_HH

# include <thread>

# include <elle/serialization/fwd.hh>

# include <cryptography/rsa/KeyPair.hh>

# include <infinit/model/blocks/MutableBlock.hh>
# include <infinit/model/doughnut/fwd.hh>
# include <infinit/serialization.hh>

namespace infinit
{
  namespace model
  {
    namespace doughnut
    {
      struct OKBDontWaitForSignature {};

      template <typename Block>
      class BaseOKB;

      struct OKBHeader
      {
      /*-------------.
      | Construction |
      `-------------*/
      public:
        OKBHeader(cryptography::rsa::KeyPair const& keys,
                  cryptography::rsa::KeyPair const& block_keys);
        OKBHeader(OKBHeader const& other);

      /*---------.
      | Contents |
      `---------*/
      public:
        blocks::ValidationResult
        validate(Address const& address) const;
        ELLE_ATTRIBUTE_R(cryptography::rsa::PublicKey, key);
        ELLE_ATTRIBUTE_R(cryptography::rsa::PublicKey, owner_key);
        ELLE_ATTRIBUTE_R(elle::Buffer, signature);
      protected:
        Address
        _hash_address() const;
        template <typename Block>
        friend class BaseOKB;

      /*--------------.
      | Serialization |
      `--------------*/
      public:
        OKBHeader(cryptography::rsa::PublicKey keys,
                  cryptography::rsa::PublicKey block_keys,
                  elle::Buffer signature);
        void
        serialize(elle::serialization::Serializer& s);
        typedef infinit::serialization_tag serialization_tag;
      };

      template <typename Block>
      class BaseOKB
        : public OKBHeader
        , public Block
      {
      /*------.
      | Types |
      `------*/
      public:
        typedef BaseOKB<Block> Self;
        typedef Block Super;

      /*-------------.
      | Construction |
      `-------------*/
      public:
        BaseOKB(Doughnut* owner);
        BaseOKB(BaseOKB const& other);
        ELLE_ATTRIBUTE_R(int, version);
        ELLE_ATTRIBUTE(elle::Buffer, signature);
        ELLE_ATTRIBUTE_R(Doughnut*, doughnut);
        friend class Doughnut;

      /*-------.
      | Clone  |
      `-------*/
      virtual
      std::unique_ptr<blocks::Block>
      clone() const override;

      /*--------.
      | Content |
      `--------*/
      public:
        virtual
        elle::Buffer const&
        data() const override;
        virtual
        void
        data(elle::Buffer data) override;
        virtual
        void
        data(std::function<void (elle::Buffer&)> transformation) override;
        ELLE_ATTRIBUTE_R(elle::Buffer, data_plain, protected);
        ELLE_ATTRIBUTE(bool, data_decrypted, protected);
      protected:
        void
        _decrypt_data() const;
        virtual
        elle::Buffer
        _decrypt_data(elle::Buffer const&) const;

      /*-----------.
      | Validation |
      `-----------*/
      protected:
        virtual
        void
        _seal() override;
        void
        _seal_okb();
        virtual
        blocks::ValidationResult
        _validate(blocks::Block const& previous) const override;
        virtual
        blocks::ValidationResult
        _validate() const override;
      protected:
        virtual
        void
        _sign(elle::serialization::SerializerOut& s) const;
        virtual
        bool
        _compare_payload(BaseOKB<Block> const& other) const;
        bool
        _check_signature(cryptography::rsa::PublicKey const& key,
                         elle::Buffer const& signature,
                         elle::Buffer const& data,
                         std::string const& name) const;

        template <typename T>
        blocks::ValidationResult
        _validate_version(
          blocks::Block const& other_,
          int T::*member,
          int version,
          std::function<bool (T const&)> const& compare) const;

        class Signer
        {
        public:
          ~Signer();
          std::unique_ptr<std::thread> thread;
          elle::Buffer to_sign;
          elle::Buffer signature;
        };
        mutable std::shared_ptr<Signer> _signer;
        elle::Buffer const& signature() const;
      private:
        elle::Buffer
        _sign() const;

      /*--------------.
      | Serialization |
      `--------------*/
      public:
        BaseOKB(elle::serialization::SerializerIn& input);
        virtual
        void
        serialize(elle::serialization::Serializer& s) override;
      private:
        class SerializationContent;
        BaseOKB(SerializationContent input);
        void
        _serialize(elle::serialization::Serializer& input);
      };

      typedef BaseOKB<blocks::MutableBlock> OKB;
    }
  }
}

# include <infinit/model/doughnut/OKB.hxx>

#endif
