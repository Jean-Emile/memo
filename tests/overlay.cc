#include <elle/test.hh>

#include <reactor/network/udp-socket.hh>
#include <reactor/network/buffer.hh>

#include <infinit/model/MissingBlock.hh>
#include <infinit/model/blocks/MutableBlock.hh>
#include <infinit/model/doughnut/ACB.hh>
#include <infinit/overlay/kouncil/Kouncil.hh>
#include <infinit/overlay/kelips/Kelips.hh>

#include "DHT.hh"

ELLE_LOG_COMPONENT("infinit.overlay.test");

using namespace infinit::model;
using namespace infinit::model::blocks;
using namespace infinit::model::doughnut;
using namespace infinit::overlay;

class TestConflictResolver
  : public DummyConflictResolver
  {
  };

inline std::unique_ptr<ConflictResolver>
tcr()
{
  return elle::make_unique<TestConflictResolver>();
}

class UTPInstrument
{
public:
  UTPInstrument(infinit::model::Endpoint endpoint)
    : server()
    , _endpoint(std::move(endpoint))
    , _thread(new reactor::Thread(elle::sprintf("%s server", this),
                                  std::bind(&UTPInstrument::_serve, this)))
  {
    this->server.bind({});
    this->_transmission.open();
  }

  reactor::network::UDPSocket server;
  ELLE_ATTRIBUTE_RX(reactor::Barrier, transmission);

private:
  ELLE_ATTRIBUTE(infinit::model::Endpoint, endpoint);
  ELLE_ATTRIBUTE(infinit::model::Endpoint, client_endpoint);
  void
  _serve()
  {
    char buf[10000];
    while (true)
    {
      try
      {
        reactor::network::UDPSocket::EndPoint ep;
        auto sz = server.receive_from(reactor::network::Buffer(buf, 10000), ep);
        reactor::wait(_transmission);
        if (ep.port() != _endpoint.port())
        {
          _client_endpoint = ep;
          server.send_to(reactor::network::Buffer(buf, sz), _endpoint.udp());
        }
        else
          server.send_to(reactor::network::Buffer(buf, sz), _client_endpoint.udp());
      }
      catch (reactor::network::Exception const& e)
      {
      }
    }
  }

  ELLE_ATTRIBUTE(reactor::Thread::unique_ptr, thread);
};

void
discover(DHT& dht, DHT& target, bool anonymous)
{
  if (anonymous)
    dht.dht->overlay()->discover(target.dht->local()->server_endpoints());
  else
    dht.dht->overlay()->discover(
      NodeLocation(target.dht->id(), target.dht->local()->server_endpoints()));
}

ELLE_TEST_SCHEDULED(
  basics, (Doughnut::OverlayBuilder, builder), (bool, anonymous))
{
  auto keys = infinit::cryptography::rsa::keypair::generate(512);
  auto storage = infinit::storage::Memory::Blocks();
  auto id = infinit::model::Address::random();
  auto make_dht_a = [&]
    {
      return DHT(
        ::id = id,
        ::keys = keys,
        ::storage = elle::make_unique<infinit::storage::Memory>(storage),
        make_overlay = builder);
    };
  auto disk = [&]
    {
      auto dht = make_dht_a();
      auto b = dht.dht->make_block<MutableBlock>(std::string("disk"));
      dht.dht->store(*b, STORE_INSERT, tcr());
      return b;
    }();
  auto dht_a = make_dht_a();
  auto before = dht_a.dht->make_block<MutableBlock>(std::string("before"));
  dht_a.dht->store(*before, STORE_INSERT, tcr());
  DHT dht_b(
    ::keys = keys, make_overlay = builder, ::storage = nullptr);
  if (anonymous)
    dht_b.dht->overlay()->discover(dht_a.dht->local()->server_endpoints());
  else
    dht_b.dht->overlay()->discover(
      NodeLocation(dht_a.dht->id(), dht_a.dht->local()->server_endpoints()));
  auto after = dht_a.dht->make_block<MutableBlock>(std::string("after"));
  dht_a.dht->store(*after, STORE_INSERT, tcr());
  ELLE_LOG("check non-existent block")
    BOOST_CHECK_THROW(
      dht_b.dht->overlay()->lookup(Address::random(), OP_FETCH),
      MissingBlock);
  ELLE_LOG("check block loaded from disk")
    BOOST_CHECK_EQUAL(
      dht_b.dht->overlay()->lookup(disk->address(), OP_FETCH).lock()->id(),
      dht_a.dht->id());
  ELLE_LOG("check block present before connection")
    BOOST_CHECK_EQUAL(
      dht_b.dht->overlay()->lookup(before->address(), OP_FETCH).lock()->id(),
      dht_a.dht->id());
  ELLE_LOG("check block inserted after connection")
    BOOST_CHECK_EQUAL(
      dht_b.dht->overlay()->lookup(after->address(), OP_FETCH).lock()->id(),
      dht_a.dht->id());
}

ELLE_TEST_SCHEDULED(
  dead_peer, (Doughnut::OverlayBuilder, builder), (bool, anonymous))
{
  auto keys = infinit::cryptography::rsa::keypair::generate(512);
  DHT dht_a(::keys = keys, make_overlay = builder, paxos = false);
  elle::With<UTPInstrument>(dht_a.dht->local()->server_endpoints()[0]) <<
    [&] (UTPInstrument& instrument)
    {
      DHT dht_b(::keys = keys, make_overlay = builder, paxos = false, ::storage=nullptr);
      infinit::model::Endpoints ep = {
        Endpoint("127.0.0.1", instrument.server.local_endpoint().port()),
      };
      if (anonymous)
        dht_b.dht->overlay()->discover(ep);
      else
        dht_b.dht->overlay()->discover(NodeLocation(dht_a.dht->id(), ep));
      // Ensure one request can go through.
      {
        auto block = dht_a.dht->make_block<MutableBlock>(std::string("block"));
        ELLE_LOG("store block")
          dht_a.dht->store(*block, STORE_INSERT, tcr());
        ELLE_LOG("lookup block")
          BOOST_CHECK_EQUAL(
            dht_b.dht->overlay()->lookup(block->address(), OP_FETCH).lock()->id(),
            dht_a.dht->id());
      }
      // Partition peer
      instrument.transmission().close();
      // Ensure we don't deadlock
      {
        auto block = dht_a.dht->make_block<MutableBlock>(std::string("block"));
        ELLE_LOG("store block")
          dht_a.dht->store(*block, STORE_INSERT, tcr());
    }
  };
}

ELLE_TEST_SCHEDULED(
  discover_endpoints, (Doughnut::OverlayBuilder, builder), (bool, anonymous))
{
  auto keys = infinit::cryptography::rsa::keypair::generate(512);
  auto id_a = infinit::model::Address::random();
  auto dht_a = elle::make_unique<DHT>(
    ::id = id_a, ::keys = keys, make_overlay = builder, paxos = false);
  Address old_address;
  ELLE_LOG("store first block")
  {
    auto block = dht_a->dht->make_block<MutableBlock>(std::string("block"));
    dht_a->dht->store(*block, STORE_INSERT, tcr());
    old_address = block->address();
  }
  DHT dht_b(
    ::keys = keys, make_overlay = builder, ::storage = nullptr);
  discover(dht_b, *dht_a, anonymous);
  ELLE_LOG("lookup block")
    BOOST_CHECK_EQUAL(
      dht_b.dht->overlay()->lookup(old_address, OP_FETCH).lock()->id(),
      id_a);
  ELLE_LOG("restart first DHT")
  {
    dht_a.reset();
    dht_a = elle::make_unique<DHT>(
      ::id = id_a, ::keys = keys, make_overlay = builder, paxos = false);
  }
  Address new_address;
  ELLE_LOG("store second block")
  {
    auto block = dht_a->dht->make_block<MutableBlock>(std::string("nblock"));
    dht_a->dht->store(*block, STORE_INSERT, tcr());
    new_address = block->address();
  }
  ELLE_LOG("lookup second block")
    BOOST_CHECK_THROW(
      dht_b.dht->overlay()->lookup(new_address, OP_FETCH).lock()->id(),
      MissingBlock);
  ELLE_LOG("discover new endpoints")
    discover(dht_b, *dht_a, anonymous);
  ELLE_LOG("lookup second block")
    BOOST_CHECK_EQUAL(
      dht_b.dht->overlay()->lookup(new_address, OP_FETCH).lock()->id(),
      id_a);
}

ELLE_TEST_SCHEDULED(
  key_cache_invalidation, (Doughnut::OverlayBuilder, builder), (bool, anonymous))
{
  infinit::storage::Memory::Blocks blocks;
  std::unique_ptr<infinit::storage::Storage> s(new infinit::storage::Memory(blocks));
  auto keys = infinit::cryptography::rsa::keypair::generate(512);
  auto id_a = infinit::model::Address::random();
  auto dht_a = elle::make_unique<DHT>(
    ::id = id_a, ::keys = keys, make_overlay = builder, paxos = false,
    ::protocol = infinit::model::doughnut::Protocol::utp,
    ::storage = std::move(s));
  int port = dht_a->dht->local()->server_endpoints()[0].port();
  DHT dht_b(
    ::keys = keys, make_overlay = builder, ::storage = nullptr,
    ::paxos = false,
    ::protocol = infinit::model::doughnut::Protocol::utp);
  discover(dht_b, *dht_a, anonymous);
  auto block = dht_a->dht->make_block<ACLBlock>(std::string("block"));
  auto& acb = dynamic_cast<infinit::model::doughnut::ACB&>(*block);
  acb.set_permissions(infinit::cryptography::rsa::keypair::generate(512).K(),
    true, true);
  acb.set_permissions(infinit::cryptography::rsa::keypair::generate(512).K(),
    true, true);
  dht_a->dht->store(*block, STORE_INSERT, tcr());
  auto b2 = dht_b.dht->fetch(block->address());
  dynamic_cast<MutableBlock*>(b2.get())->data(elle::Buffer("foo"));
  dht_b.dht->store(*b2, STORE_UPDATE, tcr());
  // brutal restart of a
  ELLE_LOG("disconnect A");
  dht_a->dht->local()->utp_server()->socket()->close();
  s.reset(new infinit::storage::Memory(blocks));
  ELLE_LOG("recreate A");
  auto dht_aa = elle::make_unique<DHT>(
    ::id = id_a, ::keys = keys, make_overlay = builder, paxos = false,
    ::protocol = infinit::model::doughnut::Protocol::utp,
    ::storage = std::move(s),
    ::port = port);
  // rebind local somewhere else or we get EBADF from local_endpoint
  dht_a->dht->local()->utp_server()->socket()->bind(
    boost::asio::ip::udp::endpoint(
      boost::asio::ip::address::from_string("127.0.0.1"), 0));
  // reconnect the Remote
  ELLE_LOG("reconnect to A");
  auto& peer = dht_b.dht->dock().peer_cache().begin()->second;
  ELLE_LOG("peer is %s", *peer);
  // force a reconnect
  dynamic_cast<infinit::model::doughnut::Remote&>(*peer).reconnect();
  // wait for it
  dynamic_cast<infinit::model::doughnut::Remote&>(*peer).connect();
  ELLE_LOG("re-store block");
  dynamic_cast<MutableBlock*>(b2.get())->data(elle::Buffer("foo"));
  BOOST_CHECK_NO_THROW(dht_b.dht->store(*b2, STORE_UPDATE, tcr()));
  ELLE_LOG("test end");
}

ELLE_TEST_SCHEDULED(
  data_spread, (Doughnut::OverlayBuilder, builder), (bool, anonymous), (bool, pax))
{
  infinit::storage::Memory::Blocks b1, b2;
  std::unique_ptr<infinit::storage::Storage> s1(new infinit::storage::Memory(b1));
  std::unique_ptr<infinit::storage::Storage> s2(new infinit::storage::Memory(b2));
  auto keys = infinit::cryptography::rsa::keypair::generate(512);
  auto id_a = infinit::model::Address::random();
  auto dht_a = elle::make_unique<DHT>(
    ::id = id_a, ::keys = keys, make_overlay = builder, paxos = pax,
    dht::consensus::rebalance_auto_expand = false,
    ::storage = std::move(s1));
  auto dht_b = elle::make_unique<DHT>(
    ::keys = keys, make_overlay = builder,
    dht::consensus::rebalance_auto_expand = false,
    ::paxos = pax,
    ::storage = std::move(s2));
  discover(*dht_b, *dht_a, anonymous);

  // client. Hard-coded replication_factor=3 if paxos is enabled
  auto client = elle::make_unique<DHT>(
    ::keys = keys, make_overlay = builder,
    ::paxos = pax,
    ::storage = nullptr);
  discover(*client, *dht_a, anonymous);
  discover(*client, *dht_b, anonymous);
  std::vector<infinit::model::Address> addrs;
  for (int i=0; i<50; ++i)
  {
    auto block = dht_a->dht->make_block<ACLBlock>(std::string("block"));
    addrs.push_back(block->address());
    client->dht->store(std::move(block), STORE_INSERT, tcr());
  }
  ELLE_LOG("stores: %s %s", b1.size(), b2.size());
  BOOST_CHECK_GE(b1.size(), 5);
  BOOST_CHECK_GE(b2.size(), 5);
  for (auto const& a: addrs)
    client->dht->fetch(a);
}

ELLE_TEST_SCHEDULED(
  chain_connect, (Doughnut::OverlayBuilder, builder), (bool, anonymous), (bool, pax))
{
  infinit::storage::Memory::Blocks b1, b2, b3;
  std::unique_ptr<infinit::storage::Storage> s1(new infinit::storage::Memory(b1));
  std::unique_ptr<infinit::storage::Storage> s2(new infinit::storage::Memory(b2));
  std::unique_ptr<infinit::storage::Storage> s3(new infinit::storage::Memory(b3));
  auto keys = infinit::cryptography::rsa::keypair::generate(512);
  auto id_a = infinit::model::Address::random();
  auto dht_a = elle::make_unique<DHT>(
    ::id = id_a, ::keys = keys, make_overlay = builder, paxos = pax,
    dht::consensus::rebalance_auto_expand = false,
    ::storage = std::move(s1));
  auto dht_b = elle::make_unique<DHT>(
    ::keys = keys, make_overlay = builder,
    dht::consensus::rebalance_auto_expand = false,
    ::paxos = pax,
    ::storage = std::move(s2));
  auto dht_c = elle::make_unique<DHT>(
    ::keys = keys, make_overlay = builder,
    dht::consensus::rebalance_auto_expand = false,
    ::paxos = pax,
    ::storage = std::move(s3));
  discover(*dht_b, *dht_a, anonymous);
  discover(*dht_c, *dht_b, anonymous);
  unsigned int pa=0, pb=0, pc=0;
  for (auto tgt: std::vector<DHT*>{dht_a.get(), dht_b.get(), dht_c.get()})
  {
    auto client = elle::make_unique<DHT>(
      ::keys = keys, make_overlay = builder,
      ::paxos = pax,
      ::storage = nullptr);
    discover(*client, *tgt, anonymous);
    // Give it some time to connect
    for (int i=0; i<10; ++i)
      reactor::yield();
    std::vector<infinit::model::Address> addrs;
    try
    {
      for (int i=0; i<50; ++i)
      {
        auto block = client->dht->make_block<ACLBlock>(std::string("block"));
        addrs.push_back(block->address());
        client->dht->store(std::move(block), STORE_INSERT, tcr());
      }
    }
    catch (elle::Error const& e)
    {
      ELLE_ERR("EXception storing blocks: %s", e);
      throw;
    }
    ELLE_LOG("stores: %s %s %s", b1.size(), b2.size(), b3.size());
    BOOST_CHECK_GE(b1.size()-pa, 5);
    BOOST_CHECK_GE(b2.size()-pb, 5);
    BOOST_CHECK_GE(b3.size()-pc, 5);
    pa = b1.size();
    pb = b2.size();
    pc = b3.size();
    for (auto const& a: addrs)
      client->dht->fetch(a);
    ELLE_LOG("teardown client");
  }
  ELLE_LOG("teardown");
}


ELLE_TEST_SCHEDULED(
  data_spread2, (Doughnut::OverlayBuilder, builder), (bool, anonymous), (bool, pax))
{
  infinit::storage::Memory::Blocks b1, b2;
  std::unique_ptr<infinit::storage::Storage> s1(new infinit::storage::Memory(b1));
  std::unique_ptr<infinit::storage::Storage> s2(new infinit::storage::Memory(b2));
  auto keys = infinit::cryptography::rsa::keypair::generate(512);
  auto id_a = infinit::model::Address::random();
  auto dht_a = elle::make_unique<DHT>(
    ::id = id_a, ::keys = keys, make_overlay = builder, paxos = pax,
    dht::consensus::rebalance_auto_expand = false,
    ::storage = std::move(s1));
  auto dht_b = elle::make_unique<DHT>(
    ::keys = keys, make_overlay = builder,
    dht::consensus::rebalance_auto_expand = false,
    ::paxos = pax,
    ::storage = std::move(s2));

  auto client = elle::make_unique<DHT>(
    ::keys = keys, make_overlay = builder,
    ::paxos = pax,
    ::storage = nullptr);
  discover(*client, *dht_a, anonymous);
  discover(*dht_a, *dht_b, anonymous);
  std::vector<infinit::model::Address> addrs;
  for (int i=0; i<50; ++i)
  {
    auto block = dht_a->dht->make_block<ACLBlock>(std::string("block"));
    addrs.push_back(block->address());
    client->dht->store(std::move(block), STORE_INSERT, tcr());
  }
  ELLE_LOG("stores: %s %s", b1.size(), b2.size());
  BOOST_CHECK_GE(b1.size(), 5);
  BOOST_CHECK_GE(b2.size(), 5);
  for (auto const& a: addrs)
    client->dht->fetch(a);
  ELLE_LOG("teardown");
}

ELLE_TEST_SUITE()
{
  elle::os::setenv("INFINIT_CONNECT_TIMEOUT", "1", 1);
  elle::os::setenv("INFINIT_SOFTFAIL_TIMEOUT", "3", 1);
  auto& master = boost::unit_test::framework::master_test_suite();
  auto const kelips_builder =
    [] (Doughnut& dht, std::shared_ptr<Local> local)
    {
      auto conf = kelips::Configuration();
      conf.query_get_retries = 4;
      conf.query_timeout_ms = 500;
      return elle::make_unique<kelips::Node>(
        conf, local, &dht);
    };
  auto const kouncil_builder =
    [] (Doughnut& dht, std::shared_ptr<Local> local)
    {
      return elle::make_unique<kouncil::Kouncil>(&dht, local);
    };
#define BOOST_NAMED_TEST_CASE(name,  test_function )                       \
boost::unit_test::make_test_case( boost::function<void ()>(test_function), \
                                  name,                                    \
                                  __FILE__, __LINE__ )


#define TEST_ANON(overlay, tname, timeout, ...)                           \
  overlay->add(BOOST_NAMED_TEST_CASE(#overlay "_" #tname "_named",      \
    std::bind(::tname, BOOST_PP_CAT(overlay, _builder), false, ##__VA_ARGS__)), 0, valgrind(timeout)); \
  overlay->add(BOOST_NAMED_TEST_CASE(#overlay "_" #tname "_anon",      \
    std::bind(::tname, BOOST_PP_CAT(overlay, _builder), true, ##__VA_ARGS__)), 0, valgrind(timeout))

#define OVERLAY(Name)                           \
  auto Name = BOOST_TEST_SUITE(#Name);          \
  master.add(Name);                             \
  TEST_ANON(Name, basics, 5);                   \
  TEST_ANON(Name, dead_peer, 5);                \
  TEST_ANON(Name, discover_endpoints, 10);      \
  TEST_ANON(Name, key_cache_invalidation, 10);  \
  TEST_ANON(Name, data_spread, 30, false);      \
  TEST_ANON(Name, data_spread2, 30, false);     \
  TEST_ANON(Name, chain_connect, 30, false);

  OVERLAY(kelips);
  OVERLAY(kouncil);
#undef OVERLAY
}

// int main()
// {
//   auto const kelips_builder =
//     [] (Doughnut& dht, std::shared_ptr<Local> local)
//     {
//       return elle::make_unique<kelips::Node>(
//         kelips::Configuration(), local, &dht);
//     };
//   dead_peer(kelips_builder, false);
// }
