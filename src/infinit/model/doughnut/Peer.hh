#ifndef INFINIT_MODEL_DOUGHNUT_PEER_HH
# define INFINIT_MODEL_DOUGHNUT_PEER_HH

# include <elle/Duration.hh>

# include <infinit/model/blocks/Block.hh>
# include <infinit/model/doughnut/fwd.hh>
# include <infinit/model/Model.hh>

namespace infinit
{
  namespace model
  {
    namespace doughnut
    {
      class Peer
        : public elle::Printable
      {
      /*-------------.
      | Construction |
      `-------------*/
      public:
        Peer(Doughnut& dht, Address id);
        virtual
        ~Peer();
        ELLE_ATTRIBUTE_R(Doughnut&, doughnut, protected);
        ELLE_ATTRIBUTE_R(Address, id, protected);

      /*-------.
      | Blocks |
      `-------*/
      public:
        virtual
        void
        store(blocks::Block const& block, StoreMode mode) = 0;
        std::unique_ptr<blocks::Block>
        fetch(Address address,
              boost::optional<int> local_version) const;
        virtual
        void
        remove(Address address, blocks::RemoveSignature rs) = 0;
      protected:
        virtual
        std::unique_ptr<blocks::Block>
        _fetch(Address address,
               boost::optional<int> local_version) const = 0;

      /*-----.
      | Keys |
      `-----*/
      public:
        cryptography::rsa::PublicKey
        resolve_key(int);
        std::vector<cryptography::rsa::PublicKey>
        resolve_keys(std::vector<int> const& ids);
        std::unordered_map<int, cryptography::rsa::PublicKey>
        resolve_all_keys();
      protected:
        virtual
        std::vector<cryptography::rsa::PublicKey>
        _resolve_keys(std::vector<int>) = 0;
        virtual
        std::unordered_map<int, cryptography::rsa::PublicKey>
        _resolve_all_keys() = 0;

      /*----------.
      | Printable |
      `----------*/
      public:
        /// Print pretty representation to \a stream.
        virtual
        void
        print(std::ostream& stream) const override;
      };
    }
  }
}

#endif
