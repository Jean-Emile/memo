#include <infinit/overlay/kademlia/kademlia.hh>

#include <elle/log.hh>

#include <elle/serialization/Serializer.hh>
#include <elle/serialization/json.hh>
#include <elle/serialization/binary.hh>
#include <elle/serialization/binary/SerializerIn.hh>
#include <elle/serialization/binary/SerializerOut.hh>

#include <cryptography/oneway.hh>
#include <reactor/network/buffer.hh>
#include <reactor/network/exception.hh>


ELLE_LOG_COMPONENT("infinit.overlay.kademlia");

typedef elle::serialization::Binary Serializer;

static inline kademlia::Time now()
{
  return std::chrono::steady_clock::now();
}

namespace elle
{
  namespace serialization
  {
    template<>
    struct Serialize<kademlia::PrettyEndpoint>
    {
      typedef std::string Type;
      static std::string convert(kademlia::PrettyEndpoint& ep)
      {
        return ep.address().to_string() + ":" + std::to_string(ep.port());
      }
      static kademlia::PrettyEndpoint convert(std::string& repr)
      {
        size_t sep = repr.find_first_of(':');
        auto addr = boost::asio::ip::address::from_string(repr.substr(0, sep));
        int port = std::stoi(repr.substr(sep + 1));
        return kademlia::PrettyEndpoint(addr, port);
      }
    };
    template<typename T>
    struct SerializeEndpoint
    {
      typedef elle::Buffer Type;
      static Type convert(T& ep)
      {
        Type res;
        auto addr = ep.address().to_v4().to_bytes();
        res.append(addr.data(), addr.size());
        unsigned short port = ep.port();
        res.append(&port, 2);
        return res;
      }
      static T convert(elle::Buffer& repr)
      {
        ELLE_ASSERT(repr.size() == 6);
        unsigned short port;
        memcpy(&port, &repr[4], 2);
        auto addr = boost::asio::ip::address_v4(
          std::array<unsigned char, 4>{{repr[0], repr[1], repr[2], repr[3]}});
        return T(addr, port);
      }
    };
    template<> struct Serialize<kademlia::Endpoint>
    : public SerializeEndpoint<kademlia::Endpoint>
    {};
    /*
    template<> struct Serialize<kelips::RpcEndpoint>
    : public SerializeEndpoint<kelips::RpcEndpoint>
    {};*/
  }
}

static std::default_random_engine gen;

namespace kademlia
{
  namespace packet
  {
    #define REGISTER(classname, type) \
    static const elle::serialization::Hierarchy<Packet>:: \
    Register<classname>   \
    _registerPacket##classname(type)

    struct Packet
    : public elle::serialization::VirtuallySerializable
    {
      Endpoint endpoint; // remote endpoint, filled by recvfrom
      Address sender;
    };
    #define SER(a, b, e) s.serialize(#e, e);
#define PACKET(name, ...)                                             \
      name() {}                                                           \
      name(elle::serialization::SerializerIn& input) {serialize(input);}  \
      void                                                                \
      serialize(elle::serialization::Serializer& s)                       \
      {                                                                   \
        BOOST_PP_SEQ_FOR_EACH(SER, _,                                     \
          BOOST_PP_VARIADIC_TO_SEQ(__VA_ARGS__))                          \
      }

    struct Ping: public Packet
    {
      PACKET(Ping, sender, remote_endpoint);
      Endpoint remote_endpoint;
    };
    REGISTER(Ping, "ping");

    struct Pong: public Packet
    {
      PACKET(Pong, sender, remote_endpoint);
      Endpoint remote_endpoint;
    };
    REGISTER(Pong, "pong");

    struct FindNode: public Packet
    {
      PACKET(FindNode, sender, target, requestId);
      int requestId;
      Address target;
    };
    REGISTER(FindNode, "findN");
    struct FindNodeReply: public Packet
    {
      PACKET(FindNodeReply, sender, requestId, nodes);
      int requestId;
      std::unordered_map<Address, Endpoint> nodes;
    };
    REGISTER(FindNodeReply, "foundN");
    struct FindValue: public FindNode
    {
      using FindNode::FindNode;
    };
    REGISTER(FindValue, "findV");
    struct FindValueReply: public Packet
    {
      PACKET(FindValueReply, sender, requestId, nodes, results);
      int requestId;
      std::unordered_map<Address, Endpoint> nodes;
      std::vector<Endpoint> results;
    };
    REGISTER(FindValueReply, "foundV");
    struct Store: public Packet
    {
      PACKET(Store, sender, key, value);
      Address key;
      std::vector<Endpoint> value;
    };
    REGISTER(Store, "store");
    #undef REGISTER
    #undef SER
    #undef PACKET

    template<typename T> elle::Buffer serialize(T const& packet)
    {
      elle::Buffer buf;
      elle::IOStream stream(buf.ostreambuf());
      Serializer::SerializerOut output(stream, false);
      output.serialize_forward((packet::Packet const&)packet);
      //const_cast<T&>(packet).serialize(output);
      return buf;
    }
  }

  template<typename E1, typename E2>
  void endpoint_to_endpoint(E1 const& src, E2& dst)
  {
    dst = E2(src.address(), src.port());
  }
  Kademlia::Kademlia(Configuration const& config,
                     std::unique_ptr<infinit::storage::Storage> storage)
  : Local(std::move(storage), config.port)
  , _config(config)
  {

    _self = _config.node_id;
    Address::Value v;
    _routes.resize(_config.address_size);
    memset(v, 0xFF, sizeof(Address::Value));
    for (int i = sizeof(v)-1; i>=0; --i)
    {
      for (int b=7; b>=0; --b)
      {
        if (_config.address_size <= i*8 + b )
          v[i] = v[i] & ~(1 << b);
        else
          goto done;
      }
    }
  done:
    _mask = Address(v);
    ELLE_TRACE("Using address mask %x", _mask);
    _socket.socket()->close();
    _socket.bind(Endpoint({}, server_endpoint().port()));

    _looper = elle::make_unique<reactor::Thread>("looper",
      [this] { this->_loop();});
    _pinger = elle::make_unique<reactor::Thread>("pinger",
      [this] { this->_ping();});
    _refresher = elle::make_unique<reactor::Thread>("refresher",
      [this] { this->_refresh();});
    for (auto const& ep: _config.bootstrap_nodes)
    {
      packet::Ping p;
      p.sender = _self;
      p.remote_endpoint = ep;
      elle::Buffer buf = serialize(p);
      send(buf, ep);
    }
    if (config.wait)
    {
      while (true)
      {
        int n = 0;
        for(auto const& e: _routes)
          n += e.size();
        ELLE_TRACE("%s: Waiting for %s nodes, got %s", *this, config.wait, n);
        if (n >= config.wait)
          break;
        reactor::sleep(1_sec);
      }
    }
    ELLE_LOG("%s: exiting ctor", *this);
  }

  void Kademlia::_loop()
  {
    elle::Buffer buf;
    while (true)
    {
      buf.size(5000);
      int n = 0;
      for(auto const& e: _routes)
        n += e.size();
      ELLE_TRACE("%s: knows %s nodes and %s hashes,", *this, n, _storage.size());
      Endpoint ep;
      size_t sz = _socket.receive_from(
          reactor::network::Buffer(buf.mutable_contents(), buf.size()),
          ep);
      buf.size(sz);
      std::unique_ptr<packet::Packet> packet;
      elle::IOStream stream(buf.istreambuf());
      Serializer::SerializerIn input(stream, false);
      try
      {
        input.serialize_forward(packet);
      }
      catch(elle::serialization::Error const& e)
      {
        ELLE_WARN("%s: Failed to deserialize packet: %s", *this, e);
        return;
      }
      packet->endpoint = ep;
      onContactSeen(packet->sender, ep);
      #define CASE(type) \
        else if (packet::type* p = dynamic_cast<packet::type*>(packet.get()))
      if (false) {}
      CASE(Ping)
      {
        (void)p;
        packet::Pong r;
        r.sender = _self;
        r.remote_endpoint = ep;
        elle::Buffer s = packet::serialize(r);
        send(s, ep);
      }
      CASE(Pong)
        onPong(p);
      CASE(FindValue)
        onFindValue(p);
      CASE(FindNode)
        onFindNode(p);
      CASE(FindNodeReply)
        onFindNodeReply(p);
      CASE(FindValueReply)
        onFindValueReply(p);
      CASE(Store)
        onStore(p);
      else
          ELLE_WARN("%s: Unknown packet type %s", *this, typeid(*p).name());
#undef CASE
    }
  }

  void Kademlia::send(elle::Buffer const& b, Endpoint e)
  {
    reactor::Lock l(_udp_send_mutex);
    ELLE_DUMP("%s: sending packet to %s\n%s", *this, e, b.string());
    _socket.send_to(reactor::network::Buffer(b.contents(), b.size()), e);
  }
  bool Kademlia::more(Address const& a, Address const& b)
  {
    if (a == b)
      return false;
    return !less(a, b);
  }
  bool Kademlia::less(Address const& a, Address const& b)
  {
    const Address::Value &aa = a.value();
    const Address::Value &bb = b.value();
    const Address::Value &mm = _mask.value();
    for (int p=sizeof(Address::Value)-1; p>=0; --p)
    {
      unsigned char va = aa[p] & mm[p];
      unsigned char vb = bb[p] & mm[p];
      if (va < vb)
        return true;
      if (va > vb)
        return false;
    }
    return false;
  }
  Address Kademlia::dist(Address const& a, Address const& b)
  {
    auto va = a.value();
    auto vb = b.value();
    auto mv = _mask.value();
    Address::Value r;
    for (unsigned int i=0; i<sizeof(r); ++i)
      r[i] = (va[i] ^ vb[i]) & mv[i];
    return Address(r);
  }
  int Kademlia::bucket_of(Address const& a)
  {
    Address d = dist(a, _config.node_id);
    auto dv = d.value();
    for (int p=sizeof(Address::Value)-1; p>=0; --p)
    {
      if (dv[p])
      {
        for (int b=7; b>=0; --b)
        {
          if (dv[p] & (1<<b))
            return p*8 + b;
        }
      }
    }
    return 0;
  }


  infinit::overlay::Overlay::Members Kademlia::_lookup(infinit::model::Address address,
                                     int n, infinit::overlay::Operation op) const
  {
    ELLE_LOG("%s: lookup %s", *this, address);
    auto self = const_cast<Kademlia*>(this);
    if (op == infinit::overlay::OP_INSERT)
    { // Lets try an insert policy of 'closest nodes'
      std::shared_ptr<Query> q =
      self->startQuery(address, false);
      ELLE_TRACE("%s: waiting for insert query", *this);
      q->barrier.wait();
      ELLE_TRACE("%s: insert query finished", *this);
      // pick the closest node found to store
      packet::Store s;
      s.sender = _self;
      s.key = address;
      s.value = {q->endpoints.at(q->res[0])};
      elle::Buffer buf = serialize(s);
      // store the mapping in the k closest nodes
      for (unsigned int i=0; i<q->res.size() && i<unsigned(_config.k); ++i)
      {
        self->send(buf, q->endpoints.at(q->res[i]));
      }
      infinit::overlay::Overlay::Members res;
      res.push_back({});
      endpoint_to_endpoint(s.value[0], res.back());
      return res;
    }

    std::shared_ptr<Query> q = self->startQuery(address, true);
    ELLE_TRACE("%s: waiting for value query", *this);
    q->barrier.wait();
    ELLE_TRACE("%s: waiting done", *this);
    if (q->storeResult.empty() && op == infinit::overlay::OP_INSERT_OR_UPDATE)
    {
      return _lookup(address, n, infinit::overlay::OP_INSERT);
    }
    infinit::overlay::Overlay::Members res;
    if (!q->storeResult.empty())
    {
      res.push_back({});
      endpoint_to_endpoint(q->storeResult[0], res.back());
    }
    return res;
  }

  static int qid = 0;
  std::shared_ptr<Kademlia::Query> Kademlia::startQuery(Address const& target, bool storage)
  {
    auto sq = std::make_shared<Query>();
    int id = ++qid;
    sq->target = target;
    sq->pending = 0;
    sq->n = 1;
    sq->steps = 0;
    _queries.insert(std::make_pair(id, sq));
    // initialize k candidates
    auto map = closest(target);
    if (map.empty())
    {
      return {};
    }
    for (auto const& e: map)
    {
      sq->endpoints[e.first] = e.second;
      sq->candidates.push_back(e.first);
    }
    sq->res = sq->candidates;
    for (int i=0; i<_config.alpha; ++i)
    {
      boost::optional<Address> a = recurseRequest(*sq, {});
      if (!a)
      {
        ELLE_TRACE("%s: startQuery %s(%s): no target", *this,
          id, storage);
        continue;
      }
      packet::FindValue fv;
      fv.sender = _self;
      fv.requestId = id;
      fv.target = target;
      elle::Buffer buf;
      if (storage)
        buf = serialize(fv);
      else
        buf = serialize(packet::FindNode(fv));
      ELLE_TRACE("%s: startquery %s(%s) send to %s",
        *this, id, storage, *a);
      send(buf, sq->endpoints.at(*a));
    }

    return sq;
  }

  void Configuration::serialize(elle::serialization::Serializer& s)
  {
    s.serialize("node_id", node_id);
    s.serialize("port", port);
    s.serialize("bootstrap_nodes", bootstrap_nodes);
    s.serialize("wait", wait);
    s.serialize("address_size", address_size);
    s.serialize("k", k);
    s.serialize("alpha", alpha);
  }

  void Kademlia::store(infinit::model::blocks::Block const& block,
                       infinit::model::StoreMode mode)
  {
    Local::store(block, mode);
    // advertise it
    ELLE_TRACE("%s: Advertizing %x", *this, block.address());
  }
  void Kademlia::remove(Address address)
  {
    Local::remove(address);
  }
  std::unique_ptr<infinit::model::blocks::Block>
  Kademlia::fetch(Address address) const
  {
    return Local::fetch(address);
  }

  void Kademlia::print(std::ostream& o) const
  {
    o << "Kad(" << _socket.local_endpoint() << ')';
  }

  void Kademlia::onContactSeen(Address sender, Endpoint ep)
  {
    int b = bucket_of(sender);
    ELLE_DUMP("%s: %x -> %s", *this, sender, b);
    ELLE_ASSERT(unsigned(b) < _routes.size());
    auto& bucket = _routes[b];
    auto it = std::find_if(bucket.begin(), bucket.end(),
      [&](Node const& a) { return a.address == sender;});
    if (it != bucket.end())
    {
      it->endpoint = ep;
      it->last_seen = now();
    }
    else if (bucket.size() < unsigned(_config.k))
    {
      ELLE_DEBUG("Inserting new node %x(%s)", sender, ep);
      bucket.push_back(Node{sender, ep, now(), 0});
    }
    else
    { // FIXME store in backup list or maybe kick a bad node
      ELLE_DEBUG("Dropping extra node %x(%s)", sender, ep);
    }
  }

  void Kademlia::onPong(packet::Pong* p)
  {
    Address addr = p->sender;
    int b = bucket_of(addr);
    auto& bucket = _routes[b];
    auto it = std::find_if(bucket.begin(), bucket.end(),
      [&](Node const& a) { return a.address == addr;});
    if (it == bucket.end())
      return;
    it->last_seen = now();
    it->endpoint = p->endpoint;
    it->unack_ping--;
  }

  std::unordered_map<Address, Endpoint> Kademlia::closest(Address addr)
  {
    std::unordered_map<Address, Endpoint> result;
    int b = bucket_of(addr);
    for (; b>= 0; --b)
    {
      auto& bucket = _routes[b];
      for (auto const& e: bucket)
      {
        result[e.address] = e.endpoint;
        if (result.size() == unsigned(_config.k))
          return result;
      }
    }
    for (b = bucket_of(addr)+1; unsigned(b) < _routes.size(); ++b)
    {
      auto& bucket = _routes[b];
      for (auto const& e: bucket)
      {
        result[e.address] = e.endpoint;
        if (result.size() == unsigned(_config.k))
          return result;
      }
    }
    return result;
  }

  void Kademlia::onFindNode(packet::FindNode* p)
  {
    std::unordered_map<Address, Endpoint> result = closest(p->target);
    packet::FindNodeReply res;
    res.sender = _self;
    res.nodes = result;
    res.requestId = p->requestId;
    elle::Buffer buf = packet::serialize(res);
    send(buf, p->endpoint);
  }

  void Kademlia::onFindValue(packet::FindValue* p)
  {
    ELLE_DEBUG("%s: onFindValue %s", *this, p->requestId);
    std::unordered_map<Address, Endpoint> result = closest(p->target);
    packet::FindValueReply res;
    res.sender = _self;
    res.nodes = result;
    res.requestId = p->requestId;
    auto it = _storage.find(p->target);
    if (it != _storage.end())
    {
      for (auto const& s: it->second)
        res.results.push_back(s.endpoint);
    }
    ELLE_DEBUG("%s: onFindValue %s replying with %s nodes and %s results",
      *this, p->requestId, res.nodes.size(), res.results.size());
    elle::Buffer buf = packet::serialize(res);
    send(buf, p->endpoint);
  }

  void Kademlia::onStore(packet::Store* p)
  {
    ELLE_DEBUG("%s: Storing %x at %x", *this, p->key, _self);
    auto& e = _storage[p->key];
    for (auto const& ep: p->value)
    {
      auto it = std::find_if(e.begin(), e.end(),
        [&](Store const& s) { return s.endpoint == ep;});
      if (it != e.end())
        it->last_seen = now();
      else
        e.push_back(Store{ep, now()});
    }
  }

  void Kademlia::finish(int rid, Query& q)
  {
    std::sort(q.res.begin(), q.res.end(),
      [&](Address const& a, Address const& b) -> bool {
        return less(dist(a, q.target), dist(b, q.target));
      });
    q.barrier.open();
    _queries.erase(rid);
  }

  boost::optional<Address> Kademlia::recurseRequest(
    Query& q,
    std::unordered_map<Address, Endpoint> const& nodes)
  {
    ++q.steps;
    for (auto const& r: nodes)
    {
      if (r.first == _self)
        continue;
      onContactSeen(r.first, r.second);
      q.endpoints[r.first] = r.second;
      if (std::find(q.queried.begin(), q.queried.end(),r.first) == q.queried.end()
        && std::find(q.candidates.begin(), q.candidates.end(), r.first) == q.candidates.end())
        q.candidates.push_back(r.first);
      if (std::find(q.res.begin(), q.res.end(), r.first) == q.res.end())
        q.res.push_back(r.first);
    }
    if (q.candidates.empty())
    {
      ELLE_TRACE("%s: no more candidates", *this);
      return {};
    }
    auto addrIt = std::min_element(q.candidates.begin(), q.candidates.end(),
      [&](Address const& a, Address const& b) -> bool {
        return less(dist(a, q.target), dist(b, q.target));
      });
    Address addr = *addrIt;
    // stop query if we already queried the k closest nodes we know about
    std::sort(q.res.begin(), q.res.end(),
      [&](Address const& a, Address const& b) -> bool {
        return less(dist(a, q.target), dist(b, q.target));
      });
    if (q.steps >= 3 && q.res.size() >= unsigned(_config.k)
      && less(dist(q.res[_config.k-1], q.target),
              dist(addr, q.target)))
    {
      ELLE_TRACE("%s: fetched enough", *this);
      return {};
    }
    std::swap(q.candidates[addrIt - q.candidates.begin()], q.candidates.back());
    q.candidates.pop_back();
    q.queried.push_back(addr);
    ++q.pending;
    return addr;
  }

  void Kademlia::onFindNodeReply(packet::FindNodeReply* p)
  {
    auto it = _queries.find(p->requestId);
    if (it == _queries.end())
    {
      ELLE_DEBUG("%s: query %s is gone", *this, p->requestId);
      return;
    }
    ELLE_DEBUG("%s: query %s got %s nodes", *this, p->requestId, p->nodes.size());
    auto& q = *it->second;
    --q.pending;
    boost::optional<Address> addr = recurseRequest(q, p->nodes);
    if (!addr)
    {
      finish(it->first, q);
      return;
    }
    ELLE_DEBUG("%s: query %s passed to %x", *this, p->requestId, *addr);
    packet::FindNode fn;
    fn.requestId = it->first;
    fn.sender = _self;
    fn.target = q.target;
    elle::Buffer buf = serialize(fn);
    send(buf, q.endpoints.at(*addr));
  }
  void Kademlia::onFindValueReply(packet::FindValueReply * p)
  {
    ELLE_DEBUG("%s: findvalue reply %s", *this, p->requestId);
    auto it = _queries.find(p->requestId);
    if (it == _queries.end())
    {
      ELLE_DEBUG("%s: query %s is gone", *this, p->requestId);
      return;
    }
    auto& q = *it->second;
    --q.pending;
    for (auto ep: p->results)
      q.storeResult.push_back(ep);
    if (q.storeResult.size() >= unsigned(q.n))
    {
      ELLE_DEBUG("%s: got enough results on %s", *this, p->requestId);
      finish(it->first, q);
      return;
    }
    boost::optional<Address> addr = recurseRequest(q, p->nodes);
    if (!addr)
    {
      ELLE_DEBUG("%s: no more peers on %s", *this, p->requestId);
      finish(it->first, q);
      return;
    }
    packet::FindValue fv;
    fv.sender = _self;
    fv.target = q.target;
    fv.requestId = it->first;
    elle::Buffer buf = serialize(fv);
    ELLE_DEBUG("%s: forwarding value query %s to %x", *this, p->requestId, *addr);
    send(buf, q.endpoints.at(*addr));
  }

  void Kademlia::_ping()
  {
    while (true)
    {
      reactor::sleep(1_sec);
      int ncount = 0;
      for (auto const& b: _routes)
        ncount += b.size();
      ELLE_DEBUG("%s: knows %s nodes", *this, ncount);
      if (ncount == 0)
        continue;
      unsigned int target = rand() % ncount;
      unsigned int p = 0;
      for (auto& b: _routes)
      {
        if (p + b.size() > target)
        {
          auto& node = b[target - p];
          node.unack_ping++;
          packet::Ping pi;
          pi.sender = _self;
          pi.remote_endpoint = node.endpoint;
          elle::Buffer buf = serialize(pi);
          send(buf, node.endpoint);
          break;
        }
        p += b.size();
      }
    }
  }
  void Kademlia::_refresh()
  {
    while (true)
    {
      ELLE_DEBUG("%s: refresh query", *this);
      auto sq = startQuery(_self, false);
      if (sq)
      {
        sq->barrier.wait();
        ELLE_DEBUG("%s: refresh query finished", *this);
      }
      reactor::sleep(10_sec);
    }
  }
}

