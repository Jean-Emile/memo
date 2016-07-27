#ifndef INFINIT_OVERLAY_KALIMERO_HH
# define INFINIT_OVERLAY_KALIMERO_HH

# include <infinit/overlay/Overlay.hh>

namespace infinit
{
  namespace overlay
  {
    class Kalimero
      : public Overlay
    {
    /*-------------.
    | Construction |
    `-------------*/
    public:
      Kalimero(model::doughnut::Doughnut* dht,
               model::Address node_id,
               std::shared_ptr<model::doughnut::Local> local);

    /*------.
    | Peers |
    `------*/
    protected:
      virtual
      void
      _discover(NodeEndpoints const& peers) override;

    /*-------.
    | Lookup |
    `-------*/
    protected:
      virtual
      reactor::Generator<WeakMember>
      _lookup(model::Address address, int n, Operation op) const override;
      virtual
      Overlay::WeakMember
      _lookup_node(model::Address address) override;
    };

    struct KalimeroConfiguration
      : public Configuration
    {
      typedef KalimeroConfiguration Self;
      typedef Configuration Super;

      KalimeroConfiguration();
      KalimeroConfiguration(elle::serialization::SerializerIn& input);
      ELLE_CLONABLE();
      void
      serialize(elle::serialization::Serializer& s) override;
      virtual
      std::unique_ptr<infinit::overlay::Overlay>
      make(model::Address id,
           NodeEndpoints const& hosts,
           std::shared_ptr<model::doughnut::Local> local,
           model::doughnut::Doughnut* doughnut) override;
    };
  }
}

#endif
