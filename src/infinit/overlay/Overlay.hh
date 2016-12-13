#ifndef INFINIT_OVERLAY_OVERLAY_HH
# define INFINIT_OVERLAY_OVERLAY_HH

# include <unordered_map>

# include <elle/Clonable.hh>
# include <elle/json/json.hh>
# include <elle/log.hh>

# include <reactor/network/tcp-socket.hh>
# include <reactor/Generator.hh>

# include <infinit/model/Address.hh>
# include <infinit/model/Endpoints.hh>
# include <infinit/model/doughnut/fwd.hh>
# include <infinit/model/doughnut/protocol.hh>
# include <infinit/serialization.hh>

namespace infinit
{
  namespace overlay
  {
    using model::Endpoints;
    using model::NodeLocation;
    using model::NodeLocations;

    enum Operation
    {
      OP_FETCH,
      OP_INSERT,
      /// Fetch faster but can return a subset of requested nodes
      OP_FETCH_FAST,
    };
    std::ostream&
    operator <<(std::ostream& output, Operation op);

    class Overlay
    {
    /*------.
    | Types |
    `------*/
    public:
      typedef std::shared_ptr<model::doughnut::Peer> Member;
      typedef std::ambivalent_ptr<model::doughnut::Peer> WeakMember;
      typedef std::vector<Member> Members;

    /*-------------.
    | Construction |
    `-------------*/
    public:
      Overlay(model::doughnut::Doughnut* dht,
              std::shared_ptr<infinit::model::doughnut::Local> local);
      virtual
      ~Overlay();
      ELLE_ATTRIBUTE_R(model::doughnut::Doughnut*, doughnut);
      ELLE_attribute_r(model::Address, id);
      ELLE_ATTRIBUTE_R(std::shared_ptr<model::doughnut::Local>, local);

    /*------.
    | Peers |
    `------*/
    public:
      void
      discover(Endpoints const& peer);
      void
      discover(std::vector<Endpoints> const& peers);
      void
      discover(NodeLocation const& peer);
      void
      discover(NodeLocations const& peers);
    protected:
      virtual
      void
      _discover(NodeLocations const& peers) = 0;

    /*------.
    | Hooks |
    `------*/
    public:
      ELLE_ATTRIBUTE_RX(
        boost::signals2::signal<void (NodeLocation id,
                                      bool observer)>, on_discover);
      ELLE_ATTRIBUTE_RX(
        boost::signals2::signal<void (model::Address id,
                                      bool observer)>, on_disappear);

    /*-------.
    | Lookup |
    `-------*/
    public:
      /// Lookup multiple addresses (OP_FETCH/UPDATE only)
      reactor::Generator<std::pair<model::Address, WeakMember>>
      lookup(std::vector<model::Address> const& addresses, int n) const;
      /// Lookup a list of nodes
      reactor::Generator<WeakMember>
      lookup(model::Address address, int n, Operation op) const;
      /// Lookup a single node
      WeakMember
      lookup(model::Address address, Operation op) const;
      /** Lookup a node from its id.
       *
       * @arg id Id of the node to lookup.
       * @raise elle::Error if the node is not found.
       */
      WeakMember
      lookup_node(model::Address id) const;
      /** Lookup nodes from their ids.
       *
       * @arg ids ids of the nodes to lookup.
       * @raise elle::Error if the node is not found.
       */
      reactor::Generator<WeakMember>
      lookup_nodes(std::unordered_set<model::Address> ids) const;
    protected:
      virtual
      reactor::Generator<std::pair<model::Address, WeakMember>>
      _lookup(std::vector<model::Address> const& addresses, int n) const;
      virtual
      reactor::Generator<WeakMember>
      _lookup(model::Address address, int n, Operation op) const = 0;
      /** Lookup a node by id
       *
       *  @raise elle::Error if the node cannot be found.
       */
      virtual
      WeakMember
      _lookup_node(model::Address address) const = 0;

    /*------.
    | Query |
    `------*/
    public:
      /// Query overlay specific informations.
      virtual
      elle::json::Json
      query(std::string const& k, boost::optional<std::string> const& v);

    /*-----------.
    | Monitoring |
    `-----------*/
    public:
      virtual
      std::string
      type_name() = 0;
      virtual
      elle::json::Array
      peer_list() = 0;
      virtual
      elle::json::Object
      stats() = 0;
    };

    struct Configuration
      : public elle::serialization::VirtuallySerializable<false>
      , public elle::Clonable<Configuration>
    {
      model::doughnut::Protocol rpc_protocol;

      Configuration();
      Configuration(elle::serialization::SerializerIn& input);
      static constexpr char const* virtually_serializable_key = "type";
      /// Perform any initialization required at join time.
      // virtual
      // void
      // join();
      void
      serialize(elle::serialization::Serializer& s) override;
      typedef infinit::serialization_tag serialization_tag;
      virtual
      std::unique_ptr<infinit::overlay::Overlay>
      make(std::shared_ptr<model::doughnut::Local> local,
           model::doughnut::Doughnut* doughnut) = 0;
    };

    /*-----------.
    | Exceptions |
    `-----------*/

    class NodeNotFound
      : public elle::Error
    {
    public:
      NodeNotFound(model::Address id);
      ELLE_ATTRIBUTE_R(model::Address, id);
    };
  }
}

#endif
