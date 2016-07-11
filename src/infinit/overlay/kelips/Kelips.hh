#ifndef INFINIT_OVERLAY_KELIPS_HH
# define INFINIT_OVERLAY_KELIPS_HH

# include <chrono>
# include <random>

# include <elle/serialization/Serializer.hh>

# include <cryptography/SecretKey.hh>

# include <reactor/Barrier.hh>
# include <reactor/Generator.hh>
# include <reactor/network/rdv-socket.hh>
# include <reactor/network/utp-server.hh>

# include <infinit/model/doughnut/Local.hh>
# include <infinit/model/doughnut/Remote.hh>
# include <infinit/overlay/Overlay.hh>
# include <infinit/storage/Storage.hh>

namespace std
{
  template<>
  struct hash<boost::asio::ip::udp::endpoint>
  {
    size_t
    operator ()(boost::asio::ip::udp::endpoint const& e) const;
  };
}

namespace infinit
{
  namespace overlay
  {
    namespace kelips
    {
      typedef model::Endpoint Endpoint;
      typedef model::Endpoints Endpoints;
      typedef model::NodeLocation NodeLocation;
      typedef infinit::model::Address Address;
      typedef std::chrono::time_point<std::chrono::system_clock> Time;
      typedef Time::duration Duration;
      //typedef std::chrono::duration<long, std::ratio<1, 1000000>> Duration;

      typedef std::pair<Endpoint, Time> TimedEndpoint;
      struct Contact
      {
        // Endpoint we last received a message from
        boost::optional<TimedEndpoint> validated_endpoint;
        // all advertised endpoints
        std::vector<TimedEndpoint> endpoints;
        Address address;
        Duration rtt;
        Time last_gossip;
        int gossip_count;
        // thread performing a contact() call on this node
        reactor::Thread::unique_ptr contacter;
        std::vector<elle::Buffer> pending;
      };

      std::ostream&
      operator << (std::ostream& output, Contact const& contact);

      struct File
      {
        Address address;
        Address home_node;
        Time last_seen;
        Time last_gossip;
        int gossip_count;
      };

      typedef std::unordered_multimap<Address, File> Files;
      typedef std::unordered_map<Address, Contact> Contacts;
      struct State
      {
        Files files;
        //contacts from all groups. We will allow contacts[_group] to grow more
        std::vector<Contacts> contacts; //contacts from other groups
        Contacts observers;
      };

      namespace packet
      {
        struct Packet;
        struct Pong;
        struct Gossip;
        struct BootstrapRequest;
        struct FileBootstrapRequest;
        struct MultiGetFileRequest;
        struct MultiGetFileReply;
        struct GetFileRequest;
        struct GetFileReply;
        struct PutFileRequest;
        struct PutFileReply;
        struct RequestKey;
      }
      struct PendingRequest;

      struct GossipConfiguration
      {
        GossipConfiguration();
        GossipConfiguration(elle::serialization::SerializerIn& input);
        void
        serialize(elle::serialization::Serializer& s);

        int interval_ms;
        /// entry considered new if gossip_count < threshold
        int new_threshold;
        /// entry considered old if last_gossip below that duration
        int old_threshold_ms;
        ///how many files per packet
        int files;
        /// how many nodes in other groups per packet
        int contacts_group;
        /// how many nodes in self group per packet
        int contacts_other;
        /// how many gossip targets in group
        int group_target;
        /// how many gossip targets in other groups
        int other_target;
        int bootstrap_group_target;
        int bootstrap_other_target;
      };

      struct Configuration
        : public overlay::Configuration
      {
        Configuration();
        Configuration(elle::serialization::SerializerIn& input);
        void
        serialize(elle::serialization::Serializer& s) override;
        /// number of groups
        int k;
        /// max number of contacts on each other group
        int max_other_contacts;
        /// query retry
        int query_get_retries;
        /// query retry
        int query_put_retries;
        /// query timeout
        int query_timeout_ms;
        /// query initial ttl
        int query_get_ttl;
        /// query initial ttl
        int query_put_ttl;
        /// query initial ttl
        int query_put_insert_ttl;
        /// entry lifetime before supression
        int contact_timeout_ms;
        /// entry lifetime before supression
        int file_timeout_ms;
        int ping_interval_ms;
        int ping_timeout_ms;
        std::vector<Endpoints> bootstrap_nodes;
        /// wait for 'wait' nodes before starting
        int wait;
        bool encrypt;
        bool accept_plain;
        infinit::model::doughnut::Protocol rpc_protocol;
        GossipConfiguration gossip;
        virtual
        std::unique_ptr<infinit::overlay::Overlay>
        make(std::vector<Endpoints> const& hosts,
             std::shared_ptr<model::doughnut::Local> server,
             model::doughnut::Doughnut* doughnut) override;
      };

      typedef std::pair<
        std::unordered_map<Address, Endpoints>, // contacts
        std::vector<std::pair<Address, Address>> // address, home_node
      > SerState;

      class Node
        : public infinit::overlay::Overlay
        , public elle::Printable
      {
      public:
        Node(Configuration const& config,
             std::shared_ptr<model::doughnut::Local> local,
             infinit::model::doughnut::Doughnut* doughnut);
        virtual ~Node();
        void
        start();
        void
        engage();
        void
        address(Address file,
                infinit::overlay::Operation op,
                int n, std::function<void(NodeLocation)> yield);
        void
        print(std::ostream& stream) const override;
        /// local hooks interface
        void
        store(infinit::model::blocks::Block const& block);
        void
        fetch(Address address,
              std::unique_ptr<infinit::model::blocks::Block>& res);
        void
        remove(Address address);
        elle::json::Json
        query(std::string const& k, boost::optional<std::string> const& v) override;

      /*--------.
      | Overlay |
      `--------*/
      protected:
        virtual
        reactor::Generator<Overlay::WeakMember>
        _lookup(infinit::model::Address address, int n,
                infinit::overlay::Operation op) const override;
        virtual
        reactor::Generator<std::pair<model::Address, Overlay::WeakMember>>
        _lookup(std::vector<infinit::model::Address> const& address, int n) const override;
        virtual
        WeakMember
        _lookup_node(Address address) override;
      private:
        typedef infinit::model::doughnut::Local Local;
        typedef infinit::overlay::Overlay Overlay;
        void
        reload_state(Local& l);
        void
        wait(int contacts);
        void
        send(packet::Packet& p, Contact& c);
        void
        send(packet::Packet& p, Endpoint ep, Address addr);
        void
        send(packet::Packet& p, Contact* c, Endpoint* ep, Address* addr);
        /// consistent address -> group mapper
        int
        group_of(Address const& address) const;
        void
        gossipListener();
        void
        gossipEmitter();
        void
        pinger();
        /// opportunistic contact grabbing
        void
        onContactSeen(Address addr, Endpoint endpoint, bool observer);
        void
        onPong(packet::Pong*);
        void
        onGossip(packet::Gossip*);
        void
        onBootstrapRequest(packet::BootstrapRequest*);
        void
        onFileBootstrapRequest(packet::FileBootstrapRequest*);
        void
        onGetFileRequest(packet::GetFileRequest*);
        void
        onGetFileReply(packet::GetFileReply*);
        void
        onMultiGetFileRequest(packet::MultiGetFileRequest*);
        void
        onMultiGetFileReply(packet::MultiGetFileReply*);
        void
        onPutFileRequest(packet::PutFileRequest*);
        void
        onPutFileReply(packet::PutFileReply*);
        void
        filterAndInsert(
          std::vector<Address> files, int target_count,
          std::unordered_map<Address, std::pair<Time, Address>>& p);
        void
        filterAndInsert(
          std::vector<Address> files, int target_count, int group,
          std::unordered_map<Address, std::vector<TimedEndpoint>>& p);
        void
        cleanup();
        void
        addLocalResults(packet::GetFileRequest* p, reactor::yielder<NodeLocation>::type const* yield);
        void
        addLocalResults(packet::MultiGetFileRequest* p,
                        reactor::yielder<std::pair<Address, NodeLocation>>::type const* yield,
                        std::vector<std::set<Address>>& result_sets);
        void
        kelipsMGet(std::vector<Address> files, int n,
                   std::function<void (std::pair<Address, NodeLocation>)> yield);
        void
        kelipsGet(Address file, int n, bool local_override, int attempts,
          bool query_node,
          bool fast_mode, // return as soon as we have a result
          std::function<void(NodeLocation)> yield);
        std::vector<NodeLocation>
        kelipsPut(Address file, int n);
        std::unordered_multimap<Address, std::pair<Time, Address>>
        pickFiles();
        std::unordered_map<Address, std::vector<TimedEndpoint>>
        pickContacts();
        std::vector<Address>
        pickOutsideTargets();
        std::vector<Address>
        pickGroupTargets();
        std::pair<infinit::cryptography::SecretKey*, bool>
        getKey(Address const& a);
        void
        setKey(Address const& a,
               infinit::cryptography::SecretKey sk,
               bool observer);
        void
        process_update(SerState const& s);
        void
        bootstrap(bool use_bootstrap_nodes,
                  bool use_contacts = true,
                  NodeLocations const& peers = {});
        void
        _discover(NodeLocations const& peers) override;
        void
        send_bootstrap(NodeLocation const& l);
        SerState
        get_serstate(NodeLocation const& peer);
        void
        contact(Address address); // establish contact with peer and flush buffer
        void onPacket(reactor::network::Buffer buf, Endpoint source);
        void process(elle::Buffer const& buf, Endpoint source);
        Contact*
        get_or_make(Address address, bool observer,
          std::vector<Endpoint> endpoints, bool make=true);
        Overlay::WeakMember
        make_peer(NodeLocation pl);
        packet::RequestKey make_key_request();
        Address _self;
        Address _ping_target;
        Time _ping_time;
        reactor::Barrier _ping_barrier;
        std::vector<TimedEndpoint> _local_endpoints;
        /// group we are in
        int _group;
        Configuration _config;
        State _state;
        reactor::network::RDVSocket _gossip;
        reactor::Mutex _udp_send_mutex;
        reactor::Thread::unique_ptr
          _emitter_thread, _listener_thread, _pinger_thread,
          _rereplicator_thread, _rdv_connect_thread, _rdv_connect_thread_local,
          _rdv_connect_gossip_thread;
        std::default_random_engine _gen;
        std::unordered_map<int, std::shared_ptr<PendingRequest>>
          _pending_requests;
        /// Addresses for which we accepted a put.
        std::vector<Address> _promised_files;
        // address -> (isObserver, key)
        std::unordered_map<Address,
          std::pair<infinit::cryptography::SecretKey, bool>> _keys;
        /// Bootstrap pending auth.
        Endpoints _pending_bootstrap_endpoints;
        std::vector<Address> _pending_bootstrap_address;
        std::vector<Address> _bootstrap_requests_sent;
        reactor::network::UTPServer _remotes_server;
        int _next_id;
        int _port;
        bool _observer;
        int _dropped_puts;
        int _dropped_gets;
        int _failed_puts;
        std::unordered_map<Address, int> _under_duplicated;
        std::string _rdv_id;
        std::string _rdv_host;
        std::unordered_map<std::string, elle::Buffer> _challenges;
        ELLE_ATTRIBUTE(
          (std::unordered_map<Address, std::vector<Overlay::WeakMember>>),
          peer_cache);
        std::unordered_map<Address,
          std::pair<reactor::Thread::unique_ptr, bool>> _node_lookups;
        std::unordered_map<reactor::Thread*, reactor::Thread::unique_ptr>
          _bootstraper_threads;
      private:
        boost::optional<model::Endpoints>
        _refetch_endpoints(model::Address id);
      };
    }
  }
}

# include <infinit/overlay/kelips/Kelips.hxx>

#endif
