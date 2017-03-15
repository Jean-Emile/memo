#ifndef INFINIT_MODEL_DOUGHNUT_CONFLICT_HH
# define INFINIT_MODEL_DOUGHNUT_CONFLICT_HH

# include <elle/Error.hh>

# include <infinit/model/blocks/Block.hh>

namespace infinit
{
  namespace model
  {
    class Conflict
      : public elle::Error
    {
    /*------.
    | Types |
    `------*/
    public:
      using Self = infinit::model::Conflict;
      using Super = elle::Error;

    /*-------------.
    | Construction |
    `-------------*/
    public:
      Conflict(std::string const& reason,
               std::unique_ptr<blocks::Block> current);
      Conflict(Conflict const& source); // FIXME: see implementation
      ELLE_ATTRIBUTE_R(std::unique_ptr<blocks::Block>, current);

    /*--------------.
    | Serialization |
    `--------------*/
    public:
      Conflict(elle::serialization::SerializerIn& input);

      void
      serialize(elle::serialization::Serializer& s,
                elle::Version const&) override;
    private:
      void
      _serialize(elle::serialization::Serializer& s);
    };
  }
}

#endif
