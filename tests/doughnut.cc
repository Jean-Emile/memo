#include <memory>

#include <boost/range/algorithm/count_if.hpp>
#include <boost/signals2/connection.hpp>

#include <elle/cast.hh>
#include <elle/filesystem/TemporaryDirectory.hh>
#include <elle/log.hh>
#include <elle/test.hh>
#include <elle/utils.hh>
#include <elle/Version.hh>

#ifndef INFINIT_WINDOWS
# include <elle/reactor/network/unix-domain-socket.hh>
#endif

#include <infinit/model/Conflict.hh>
#include <infinit/model/MissingBlock.hh>
#include <infinit/model/MonitoringServer.hh>
#include <infinit/model/blocks/ACLBlock.hh>
#include <infinit/model/blocks/ImmutableBlock.hh>
#include <infinit/model/blocks/MutableBlock.hh>
#include <infinit/model/doughnut/ACB.hh>
#include <infinit/model/doughnut/Cache.hh>
#include <infinit/model/doughnut/Doughnut.hh>
#include <infinit/model/doughnut/Group.hh>
#include <infinit/model/doughnut/Local.hh>
#include <infinit/model/doughnut/NB.hh>
#include <infinit/model/doughnut/Remote.hh>
#include <infinit/model/doughnut/UB.hh>
#include <infinit/model/doughnut/User.hh>
#include <infinit/model/doughnut/ValidationFailed.hh>
#include <infinit/model/doughnut/consensus/Paxos.hh>
#include <infinit/overlay/Stonehenge.hh>
#include <infinit/storage/Memory.hh>

#include "DHT.hh"

ELLE_LOG_COMPONENT("infinit.model.doughnut.test");

namespace blocks = infinit::model::blocks;
namespace dht = infinit::model::doughnut;
using namespace infinit::storage;

ELLE_DAS_SYMBOL(keys_a);
ELLE_DAS_SYMBOL(keys_b);
ELLE_DAS_SYMBOL(keys_c);
ELLE_DAS_SYMBOL(id_a);
ELLE_DAS_SYMBOL(id_b);
ELLE_DAS_SYMBOL(id_c);
ELLE_DAS_SYMBOL(storage_a);
ELLE_DAS_SYMBOL(storage_b);
ELLE_DAS_SYMBOL(storage_c);
ELLE_DAS_SYMBOL(version_a);
ELLE_DAS_SYMBOL(version_b);
ELLE_DAS_SYMBOL(version_c);
ELLE_DAS_SYMBOL(monitoring_socket_path_a);
ELLE_DAS_SYMBOL(encrypt_options);

static
int
key_size()
{
  return RUNNING_ON_VALGRIND ? 512 : 2048;
}

class DHTs
{
public:
  template <typename ... Args>
  DHTs(Args&& ... args)
  {
    elle::das::named::prototype(
      paxos = true,
      ::keys_a = elle::cryptography::rsa::keypair::generate(key_size()),
      ::keys_b = elle::cryptography::rsa::keypair::generate(key_size()),
      ::keys_c = elle::cryptography::rsa::keypair::generate(key_size()),
      id_a = infinit::model::Address::random(0), // FIXME
      id_b = infinit::model::Address::random(0), // FIXME
      id_c = infinit::model::Address::random(0), // FIXME
      storage_a = nullptr,
      storage_b = nullptr,
      storage_c = nullptr,
      version_a = boost::optional<elle::Version>(),
      version_b = boost::optional<elle::Version>(),
      version_c = boost::optional<elle::Version>(),
      monitoring_socket_path_a = boost::optional<boost::filesystem::path>(),
      encrypt_options = infinit::model::doughnut::EncryptOptions(),
      make_overlay =
      [] (int,
          infinit::model::NodeLocations peers,
          std::shared_ptr<infinit::model::doughnut::Local> local,
          infinit::model::doughnut::Doughnut& d)
      {
        return std::make_unique<infinit::overlay::Stonehenge>(
          peers, std::move(local), &d);
      },
      make_consensus =
      [] (std::unique_ptr<dht::consensus::Consensus> c)
        -> std::unique_ptr<dht::consensus::Consensus>
      {
        return c;
      }
      ).call([this] (
        bool paxos,
        elle::cryptography::rsa::KeyPair keys_a,
        elle::cryptography::rsa::KeyPair keys_b,
        elle::cryptography::rsa::KeyPair keys_c,
        infinit::model::Address id_a,
        infinit::model::Address id_b,
        infinit::model::Address id_c,
        std::unique_ptr<Storage> storage_a,
        std::unique_ptr<Storage> storage_b,
        std::unique_ptr<Storage> storage_c,
        boost::optional<elle::Version> version_a,
        boost::optional<elle::Version> version_b,
        boost::optional<elle::Version> version_c,
        boost::optional<boost::filesystem::path> monitoring_socket_path_a,
        infinit::model::doughnut::EncryptOptions encrypt_options,
        std::function<
          std::unique_ptr<infinit::overlay::Stonehenge>(
            int,
            infinit::model::NodeLocations peers,
            std::shared_ptr<
              infinit::model::doughnut::Local> local,
            infinit::model::doughnut::Doughnut& d)> make_overlay,
        std::function<
          std::unique_ptr<dht::consensus::Consensus>(
            std::unique_ptr<dht::consensus::Consensus>
            )> make_consensus)
              {
                this->init(paxos,
                           std::move(keys_a),
                           std::move(keys_b),
                           std::move(keys_c),
                           id_a, id_b, id_c,
                           std::move(storage_a),
                           std::move(storage_b),
                           std::move(storage_c) ,
                           version_a, version_b, version_c,
                           std::move(monitoring_socket_path_a),
                           std::move(encrypt_options),
                           std::move(make_overlay),
                           std::move(make_consensus));
              }, std::forward<Args>(args)...);
  }

  std::shared_ptr<elle::cryptography::rsa::KeyPair> keys_a;
  std::shared_ptr<elle::cryptography::rsa::KeyPair> keys_b;
  std::shared_ptr<elle::cryptography::rsa::KeyPair> keys_c;
  std::shared_ptr<dht::Doughnut> dht_a;
  std::shared_ptr<dht::Doughnut> dht_b;
  std::shared_ptr<dht::Doughnut> dht_c;

private:
  void
  init(bool paxos,
       elle::cryptography::rsa::KeyPair keys_a,
       elle::cryptography::rsa::KeyPair keys_b,
       elle::cryptography::rsa::KeyPair keys_c,
       infinit::model::Address id_a,
       infinit::model::Address id_b,
       infinit::model::Address id_c,
       std::unique_ptr<Storage> storage_a,
       std::unique_ptr<Storage> storage_b,
       std::unique_ptr<Storage> storage_c,
       boost::optional<elle::Version> version_a,
       boost::optional<elle::Version> version_b,
       boost::optional<elle::Version> version_c,
       boost::optional<boost::filesystem::path> monitoring_socket_path_a,
       infinit::model::doughnut::EncryptOptions encrypt_options,
       std::function<
         std::unique_ptr<infinit::overlay::Stonehenge>(
           int,
           infinit::model::NodeLocations peers,
           std::shared_ptr<infinit::model::doughnut::Local> local,
           infinit::model::doughnut::Doughnut& d)> make_overlay,
       std::function<
         std::unique_ptr<dht::consensus::Consensus>(
           std::unique_ptr<dht::consensus::Consensus>)> make_consensus)
  {
    auto const consensus = [&paxos, &make_consensus] () -> dht::Doughnut::ConsensusBuilder
      {
        if (paxos)
          return
            [&] (dht::Doughnut& dht)
            {
              return make_consensus(
                std::make_unique<dht::consensus::Paxos>(dht, 3));
            };
        else
          return
            [&] (dht::Doughnut& dht)
            {
              return make_consensus(
                std::make_unique<dht::consensus::Consensus>(dht));
            };
      }();
    auto const members = infinit::model::NodeLocations
      {
        {id_a, infinit::model::Endpoints()},
        {id_b, infinit::model::Endpoints()},
        {id_c, infinit::model::Endpoints()},
      };
    std::vector<infinit::overlay::Stonehenge*> stonehenges;
    make_overlay =
      [make_overlay, &stonehenges] (
        int n,
        infinit::model::NodeLocations peers,
        std::shared_ptr<infinit::model::doughnut::Local> local,
        infinit::model::doughnut::Doughnut& d)
      {
        auto res = make_overlay(
          n, std::move(peers), std::move(local), d);
        stonehenges.emplace_back(res.get());
        return res;
      };
    // dht_a.
    {
      this->keys_a =
        std::make_shared<elle::cryptography::rsa::KeyPair>(std::move(keys_a));
      if (!storage_a)
        storage_a = std::make_unique<Memory>();
      auto const passport_a = dht::Passport{
        this->keys_a->K(), "network-name", *this->keys_a};
      this->dht_a = std::make_shared<dht::Doughnut>(
        id_a,
        this->keys_a,
        this->keys_a->public_key(),
        passport_a,
        consensus,
        infinit::model::doughnut::Doughnut::OverlayBuilder(
          [=] (infinit::model::doughnut::Doughnut& d,
               std::shared_ptr<infinit::model::doughnut::Local> local)
          {
            return make_overlay(0, members, std::move(local), d);
          }),
        boost::optional<int>(),
        boost::optional<boost::asio::ip::address>(),
        std::move(storage_a),
        dht::version = version_a,
        infinit::model::doughnut::monitoring_socket_path =
          monitoring_socket_path_a,
        infinit::model::doughnut::encrypt_options = encrypt_options);
    }
    // dht_b.
    {
      this->keys_b =
        std::make_shared<elle::cryptography::rsa::KeyPair>(std::move(keys_b));
      if (!storage_b)
        storage_b = std::make_unique<Memory>();
      auto const passport_b = dht::Passport{
        this->keys_b->K(), "network-name", *this->keys_a};
      this->dht_b = std::make_shared<dht::Doughnut>(
        id_b,
        this->keys_b,
        this->keys_a->public_key(),
        passport_b,
        consensus,
        infinit::model::doughnut::Doughnut::OverlayBuilder(
          [=] (infinit::model::doughnut::Doughnut& d,
               std::shared_ptr<infinit::model::doughnut::Local> local)
          {
            return make_overlay(1, members, std::move(local), d);
          }),
        boost::optional<int>(),
        boost::optional<boost::asio::ip::address>(),
        std::move(storage_b),
        dht::version = version_b,
        infinit::model::doughnut::encrypt_options = encrypt_options);
    }
    // dht_c.
    {
      this->keys_c =
        std::make_shared<elle::cryptography::rsa::KeyPair>(std::move(keys_c));
      if (!storage_c)
        storage_c = std::make_unique<Memory>();
      auto const passport_c = dht::Passport{
        this->keys_c->K(), "network-name", *this->keys_a};
      this->dht_c = std::make_shared<dht::Doughnut>(
        id_c,
        this->keys_c,
        this->keys_a->public_key(),
        passport_c,
        consensus,
        infinit::model::doughnut::Doughnut::OverlayBuilder(
          [=] (infinit::model::doughnut::Doughnut& d,
               std::shared_ptr<infinit::model::doughnut::Local> local)
          {
            return make_overlay(2, members, std::move(local), d);
          }),
        boost::optional<int>(),
        boost::optional<boost::asio::ip::address>(),
        std::move(storage_c),
        dht::version = version_c,
        infinit::model::doughnut::encrypt_options = encrypt_options);
    }
    for (auto* stonehenge: stonehenges)
      for (auto& peer: stonehenge->peers())
      {
        auto const dht = [this, &peer, &id_a, &id_b, &id_c]
          {
            if (peer.id() == id_a)
              return this->dht_a;
            else if (peer.id() == id_b)
              return this->dht_b;
            else if (peer.id() == id_c)
              return this->dht_c;
            else
              ELLE_ABORT("unknown doughnut id: %f", peer.id());
          }();
        elle::unconst(peer.endpoints()).emplace(
          boost::asio::ip::address::from_string("127.0.0.1"),
          dht->local()->server_endpoint().port());
      }
  }
};

template<typename C>
int
mutable_block_count(C const& c)
{
  return boost::count_if(c,
                         [](auto const& i)
                         {
                           return i.mutable_block();
                         });
}

ELLE_TEST_SCHEDULED(CHB, (bool, paxos))
{
  DHTs dhts(paxos);
  auto& dht = *dhts.dht_a;
  {
    auto data = elle::Buffer("\\_o<");
    auto block = dht.make_block<blocks::ImmutableBlock>(data);
    auto addr = block->address();
    ELLE_LOG("store block")
      dht.seal_and_insert(*block);
    ELLE_LOG("fetch block")
      BOOST_CHECK_EQUAL(dht.fetch(addr)->data(), data);
    ELLE_LOG("remove block")
      dht.remove(addr);
  }
}

ELLE_TEST_SCHEDULED(OKB, (bool, paxos))
{
  DHTs dhts(paxos);
  auto& dht = *dhts.dht_a;
  {
    auto block = dht.make_block<blocks::MutableBlock>();
    auto data = elle::Buffer("\\_o<");
    block->data(elle::Buffer(data));
    auto addr = block->address();
    ELLE_LOG("store mutable block")
      dht.seal_and_insert(*block);
    auto updated = elle::Buffer(">o_/");
    block->data(elle::Buffer(updated));
    ELLE_LOG("fetch block")
      ELLE_ASSERT_EQ(dht.fetch(addr)->data(), data);
    ELLE_LOG("store updated mutable block")
      dht.seal_and_update(*block);
    ELLE_LOG("fetch block")
      ELLE_ASSERT_EQ(dht.fetch(addr)->data(), updated);
    ELLE_LOG("remove block")
      dht.remove(addr);
  }
}

ELLE_TEST_SCHEDULED(async, (bool, paxos))
{
  DHTs dhts(paxos);
  auto& dht = *dhts.dht_c;
  {
    auto data = elle::Buffer("\\_o<");
    auto block = dht.make_block<blocks::ImmutableBlock>(data);
    std::vector<std::unique_ptr<blocks::ImmutableBlock>> blocks_;
    for (int i = 0; i < 10; ++i)
    {
      auto s = elle::sprintf("\\_o< %d", i);
      auto data = elle::Buffer(elle::sprintf(s).c_str(),
                        (int)std::strlen(s.c_str()));
      blocks_.push_back(dht.make_block<blocks::ImmutableBlock>(data));
    }
    ELLE_LOG("store block")
      dht.seal_and_insert(*block);
    for (auto& block: blocks_)
      dht.seal_and_insert(*block);
    ELLE_LOG("fetch block")
      ELLE_ASSERT_EQ(dht.fetch(block->address())->data(), data);
    for (auto& block: blocks_)
      dht.fetch(block->address());
    ELLE_LOG("remove block")
      dht.remove(block->address());
  }
  {
    auto block = dht.make_block<blocks::MutableBlock>();
    auto data = elle::Buffer("\\_o<");
    block->data(elle::Buffer(data));
    ELLE_LOG("store block")
      dht.seal_and_insert(*block);
    auto updated = elle::Buffer(">o_/");
    block->data(elle::Buffer(updated));
    ELLE_LOG("fetch block")
      ELLE_ASSERT_EQ(dht.fetch(block->address())->data(), data);
    ELLE_LOG("store block")
      dht.seal_and_update(*block);
    ELLE_LOG("fetch block")
      ELLE_ASSERT_EQ(dht.fetch(block->address())->data(), updated);
    ELLE_LOG("remove block")
      dht.remove(block->address());
  }
}

ELLE_TEST_SCHEDULED(ACB, (bool, paxos))
{
  DHTs dhts(paxos);
  auto block = dhts.dht_a->make_block<blocks::ACLBlock>();
  auto data = elle::Buffer("\\_o<");
  block->data(elle::Buffer(data));
  ELLE_LOG("owner: store ACB")
    dhts.dht_a->seal_and_insert(*block);
  {
    ELLE_LOG("other: fetch ACB");
    auto fetched = dhts.dht_b->fetch(block->address());
    BOOST_CHECK_THROW(fetched->data(), elle::Error);
    auto acb = elle::cast<blocks::ACLBlock>::runtime(fetched);
    acb->data(elle::Buffer(":-("));
    ELLE_LOG("other: stored edited ACB")
      BOOST_CHECK_THROW(dhts.dht_b->seal_and_update(*acb),
                        dht::ValidationFailed);
  }
  ELLE_LOG("owner: add ACB read permissions")
    block->set_permissions(dht::User(dhts.keys_b->K(), ""), true, false);
  ELLE_LOG("owner: store ACB")
    dhts.dht_a->seal_and_update(*block);
  {
    ELLE_LOG("other: fetch ACB");
    auto fetched = dhts.dht_b->fetch(block->address());
    BOOST_CHECK_EQUAL(fetched->data(), "\\_o<");
    auto acb = elle::cast<blocks::ACLBlock>::runtime(fetched);
    acb->data(elle::Buffer(":-("));
    ELLE_LOG("other: stored edited ACB")
      BOOST_CHECK_THROW(dhts.dht_b->seal_and_update(*acb),
                        dht::ValidationFailed);
  }
  ELLE_LOG("owner: add ACB write permissions")
    block->set_permissions(dht::User(dhts.keys_b->K(), ""), true, true);
  ELLE_LOG("owner: store ACB")
    dhts.dht_a->seal_and_update(*block);
  {
    ELLE_LOG("other: fetch ACB");
    auto fetched = dhts.dht_b->fetch(block->address());
    BOOST_CHECK_EQUAL(fetched->data(), "\\_o<");
    auto acb = elle::cast<blocks::ACLBlock>::runtime(fetched);
    acb->data(elle::Buffer(":-)"));
    ELLE_LOG("other: stored edited ACB")
      dhts.dht_b->seal_and_update(*acb);
  }
  ELLE_LOG("owner: fetch ACB")
  {
    auto fetched = dhts.dht_a->fetch(block->address());
    BOOST_CHECK_EQUAL(fetched->data(), ":-)");
  }
}

ELLE_TEST_SCHEDULED(NB, (bool, paxos))
{
  DHTs dhts(paxos);
  auto block = std::make_unique<dht::NB>(
    *dhts.dht_a, "blockname", elle::Buffer("blockdata"));
  ELLE_LOG("owner: store NB")
    dhts.dht_a->seal_and_insert(*block);
  {
    ELLE_LOG("other: fetch NB");
    auto fetched = dhts.dht_b->fetch(
      dht::NB::address(dhts.keys_a->K(), "blockname", dhts.dht_b->version()));
    BOOST_CHECK_EQUAL(fetched->data(), "blockdata");
    auto nb = elle::cast<dht::NB>::runtime(fetched);
    BOOST_CHECK(nb);
  }
  { // overwrite
    auto block = std::make_unique<dht::NB>(
      *dhts.dht_a, "blockname", elle::Buffer("blockdatb"));
    BOOST_CHECK_THROW(dhts.dht_a->seal_and_update(*block), std::exception);
  }
  // remove and remove protection
  BOOST_CHECK_THROW(
    dhts.dht_a->remove(dht::NB::address(dhts.keys_a->K(), "blockname",
                                        dhts.dht_a->version()),
                       infinit::model::blocks::RemoveSignature()),
    std::exception);
  BOOST_CHECK_THROW(
    dhts.dht_b->remove(dht::NB::address(dhts.keys_a->K(), "blockname",
                                        dhts.dht_b->version())),
    std::exception);
  dhts.dht_a->remove(dht::NB::address(dhts.keys_a->K(), "blockname",
                                      dhts.dht_a->version()));
}

ELLE_TEST_SCHEDULED(UB, (bool, paxos))
{
  DHTs dhts(paxos);
  auto& dhta = dhts.dht_a;
  auto& dhtb = dhts.dht_b;
  ELLE_LOG("store UB and RUB")
  {
    dht::UB uba(dhta.get(), "a", dhta->keys().K());
    dht::UB ubarev(dhta.get(), "a", dhta->keys().K(), true);
    dhta->seal_and_insert(uba);
    dhta->seal_and_insert(ubarev);
  }
  auto ruba = dhta->fetch(dht::UB::hash_address(dhta->keys().K(), *dhta));
  BOOST_CHECK(ruba);
  auto* uba = dynamic_cast<dht::UB*>(ruba.get());
  BOOST_CHECK(uba);
  dht::UB ubf(dhta.get(), "duck", dhta->keys().K(), true);
  ELLE_LOG("fail storing different UB")
  {
    BOOST_CHECK_THROW(dhta->seal_and_insert(ubf), std::exception);
    BOOST_CHECK_THROW(dhtb->seal_and_insert(ubf), std::exception);
  }
  ELLE_LOG("fail removing RUB")
  {
    BOOST_CHECK_THROW(dhtb->remove(ruba->address()), std::exception);
    BOOST_CHECK_THROW(
      dhtb->remove(ruba->address(), infinit::model::blocks::RemoveSignature()),
      std::exception);
    BOOST_CHECK_THROW(
      dhta->remove(ruba->address(), infinit::model::blocks::RemoveSignature()),
      std::exception);
  }
  ELLE_LOG("remove RUB")
    dhta->remove(ruba->address());
  ELLE_LOG("store different UB")
    dhtb->seal_and_insert(ubf);
}

namespace removal
{
  ELLE_TEST_SCHEDULED(serialize_ACB_remove, (bool, paxos))
  {
    Memory::Blocks dht_storage;
    auto dht_id = infinit::model::Address::random();
    infinit::model::Address address;
    // Store signature removal in the first run so the second run of the DHT
    // does not fetch the block before removing it. This tests the block is
    // still reloaded without a previous fetch.
    elle::Buffer rs_bad;
    elle::Buffer rs_good;
    ELLE_LOG("store block")
    {
      auto dht = DHT(id = dht_id,
              storage = std::make_unique<Memory>(dht_storage));
      auto b = dht.dht->make_block<blocks::ACLBlock>();
      address = b->address();
      b->data(std::string("removal/serialize_ACB_remove"));
      dht.dht->seal_and_insert(*b);
      rs_bad =
        elle::serialization::binary::serialize(b->sign_remove(*dht.dht));
      b->data([] (elle::Buffer& b) { b.append("_", 1); });
      dht.dht->seal_and_update(*b);
      rs_good =
        elle::serialization::binary::serialize(b->sign_remove(*dht.dht));
    }
    ELLE_LOG("fail removing block")
    {
      auto dht = DHT(id = dht_id,
              storage = std::make_unique<Memory>(dht_storage));
      elle::serialization::Context ctx;
      ctx.set<infinit::model::doughnut::Doughnut*>(dht.dht.get());
      auto sig = elle::serialization::binary::deserialize<
        blocks::RemoveSignature>(rs_bad, true, ctx);
      // we send a remove with an obsolete signature, so consensus will retry,
      // but will fail to call sign_remove() since we don't have the proper keys
      BOOST_CHECK_THROW(dht.dht->remove(address, sig),
                        infinit::model::doughnut::ValidationFailed);
    }
    ELLE_LOG("remove block")
    {
      auto dht = DHT(id = dht_id,
              storage = std::make_unique<Memory>(dht_storage));
      elle::serialization::Context ctx;
      ctx.set<infinit::model::doughnut::Doughnut*>(dht.dht.get());
      auto sig = elle::serialization::binary::deserialize<
        blocks::RemoveSignature>(rs_good, true, ctx);
      dht.dht->remove(address, sig);
    }
    BOOST_CHECK(!contains(dht_storage, address));
  }
}

class AppendConflictResolver
  : public infinit::model::ConflictResolver
{
  std::unique_ptr<blocks::Block>
  operator () (blocks::Block& old,
               blocks::Block& current) override
  {
    auto res = std::dynamic_pointer_cast<blocks::MutableBlock>(current.clone());
    res->data([] (elle::Buffer& data) { data.append("B", 1); });
    return std::unique_ptr<blocks::Block>(res.release());
  }

  std::string
  description() const override
  {
    return "Append data to block";
  }

  void
  serialize(elle::serialization::Serializer& s,
            elle::Version const&) override
  {}
};

ELLE_TEST_SCHEDULED(conflict, (bool, paxos))
{
  DHTs dhts(paxos);
  std::unique_ptr<blocks::ACLBlock> block_alice;
  ELLE_LOG("alice: create block")
  {
    block_alice = dhts.dht_a->make_block<blocks::ACLBlock>();
    block_alice->data(elle::Buffer("A"));
    block_alice->set_permissions(
      dht::User(dhts.keys_b->K(), "bob"), true, true);
  }
  ELLE_LOG("alice: store block")
    dhts.dht_a->seal_and_insert(*block_alice);
  std::unique_ptr<
    blocks::ACLBlock,
    std::default_delete<blocks::Block>> block_bob;
  ELLE_LOG("bob: fetch block");
  {
    block_bob = std::static_pointer_cast<blocks::ACLBlock>
      (dhts.dht_b->fetch(block_alice->address()));
    BOOST_CHECK_EQUAL(block_bob->data(), "A");
  }
  ELLE_LOG("alice: modify block")
  {
    block_alice->data(elle::Buffer("AA"));
    dhts.dht_a->seal_and_update(*block_alice);
  }
  ELLE_LOG("bob: modify block")
  {
    block_bob->data(elle::Buffer("AB"));
    BOOST_CHECK_THROW(
      dhts.dht_b->seal_and_update(*block_bob),
      infinit::model::Conflict);
    dhts.dht_b->seal_and_update(*block_bob,
                                std::make_unique<AppendConflictResolver>());
  }
  ELLE_LOG("alice: fetch block")
  {
    BOOST_CHECK_EQUAL(
      dhts.dht_a->fetch(block_alice->address())->data(), "AAB");
  }
}

void
noop(Storage*)
{}

ELLE_TEST_SCHEDULED(restart, (bool, paxos))
{
  auto keys_a = elle::cryptography::rsa::keypair::generate(key_size());
  auto keys_b = elle::cryptography::rsa::keypair::generate(key_size());
  auto keys_c = elle::cryptography::rsa::keypair::generate(key_size());
  auto id_a = infinit::model::Address::random(0); // FIXME
  auto id_b = infinit::model::Address::random(0); // FIXME
  auto id_c = infinit::model::Address::random(0); // FIXME
  Memory::Blocks storage_a;
  Memory::Blocks storage_b;
  Memory::Blocks storage_c;
  // std::unique_ptr<blocks::ImmutableBlock> iblock;
  std::unique_ptr<blocks::MutableBlock> mblock;
  ELLE_LOG("store blocks")
  {
    DHTs dhts(
      paxos,
      keys_a,
      keys_b,
      keys_c,
      id_a,
      id_b,
      id_c,
      std::make_unique<Memory>(storage_a),
      std::make_unique<Memory>(storage_b),
      std::make_unique<Memory>(storage_c)
      );
    // iblock =
    //   dhts.dht_a->make_block<blocks::ImmutableBlock>(
    //     elle::Buffer("immutable", 9));
    // dhts.dht_a->store(*iblock);
    mblock =
      dhts.dht_a->make_block<blocks::MutableBlock>(
        elle::Buffer("mutable"));
    dhts.dht_a->seal_and_insert(*mblock);
  }
  ELLE_LOG("load blocks")
  {
    DHTs dhts(
      paxos,
      keys_a,
      keys_b,
      keys_c,
      id_a,
      id_b,
      id_c,
      std::make_unique<Memory>(storage_a),
      std::make_unique<Memory>(storage_b),
      std::make_unique<Memory>(storage_c)
      );
    // auto ifetched = dhts.dht_a->fetch(iblock->address());
    // BOOST_CHECK_EQUAL(iblock->data(), ifetched->data());
    auto mfetched = dhts.dht_a->fetch(mblock->address());
    BOOST_CHECK_EQUAL(mblock->data(), mfetched->data());
  }
}

/*--------------------.
| Paxos: wrong quorum |
`--------------------*/

// Make one of the overlay return a partial quorum, missing one of the three
// members, and check it gets fixed.

class WrongQuorumStonehenge
  : public infinit::overlay::Stonehenge
{
public:
  template <typename ... Args>
  WrongQuorumStonehenge(Args&& ... args)
    : infinit::overlay::Stonehenge(std::forward<Args>(args)...)
    , fail(false)
  {}

  elle::reactor::Generator<infinit::overlay::Overlay::WeakMember>
  _lookup(infinit::model::Address address, int n, bool fast) const override
  {
    return infinit::overlay::Stonehenge::_lookup(address,
                                                 fail ? n - 1 : n,
                                                 fast);
  }

  bool fail;
};

ELLE_TEST_SCHEDULED(wrong_quorum)
{
  WrongQuorumStonehenge* stonehenge;
  DHTs dhts(
    make_overlay =
    [&stonehenge] (int dht,
                   infinit::model::NodeLocations peers,
                   std::shared_ptr<infinit::model::doughnut::Local> local,
                   infinit::model::doughnut::Doughnut& d)
    {
      if (dht == 0)
      {
        stonehenge = new WrongQuorumStonehenge(peers, std::move(local), &d);
        return std::unique_ptr<infinit::overlay::Stonehenge>(stonehenge);
      }
      else
        return std::make_unique<infinit::overlay::Stonehenge>(
          peers, std::move(local), &d);
    });
  auto block = dhts.dht_a->make_block<blocks::MutableBlock>();
  {
    auto data = elle::Buffer("\\_o<");
    block->data(elle::Buffer(data));
    ELLE_LOG("store block")
      dhts.dht_a->seal_and_insert(*block);
    auto updated = elle::Buffer(">o_/");
    block->data(elle::Buffer(updated));
    stonehenge->fail = true;
    ELLE_LOG("store updated block")
      dhts.dht_a->seal_and_update(*block);
  }
}

namespace tests_paxos
{
  ELLE_TEST_SCHEDULED(CHB_no_peer)
  {
    auto dht = DHT(storage = nullptr);
    auto chb = dht.dht->make_block<blocks::ImmutableBlock>();
    BOOST_CHECK_THROW(dht.dht->seal_and_insert(*chb),
                      elle::Error);
  }
}

ELLE_TEST_SCHEDULED(cache, (bool, paxos))
{
  dht::consensus::Cache* cache = nullptr;
  DHTs dhts(
    paxos,
    make_consensus =
      [&] (std::unique_ptr<dht::consensus::Consensus> c)
      {
        auto res = std::make_unique<dht::consensus::Cache>(std::move(c));
        if (!cache)
          cache = res.get();
        return res;
      });
  // Check a null block is never stored in cache.
  {
    auto block = dhts.dht_a->make_block<blocks::MutableBlock>();
    auto data = elle::Buffer("cached");
    block->data(elle::Buffer(data));
    auto addr = block->address();
    ELLE_LOG("store block")
      dhts.dht_a->seal_and_insert(*block);
    auto fetched = [&]
      {
        ELLE_LOG("fetch block")
          return dhts.dht_a->fetch(addr);
      }();
    BOOST_CHECK_EQUAL(block->data(), fetched->data());
    ELLE_LOG("fetch cached block")
      BOOST_CHECK(!dhts.dht_a->fetch(addr, block->version()));
    ELLE_LOG("clear cache")
      cache->clear();
    ELLE_LOG("fetch cached block")
      BOOST_CHECK(!dhts.dht_a->fetch(addr, block->version()));
    ELLE_LOG("fetch block")
      BOOST_CHECK_EQUAL(dhts.dht_a->fetch(addr)->data(), block->data());
  }
}

static std::unique_ptr<blocks::Block>
cycle(infinit::model::doughnut::Doughnut& dht,
      std::unique_ptr<blocks::Block> b)
{
  elle::Buffer buf;
  {
    elle::IOStream os(buf.ostreambuf());
    elle::serialization::binary::SerializerOut sout(os, false);
    sout.set_context(infinit::model::doughnut::ACBDontWaitForSignature{});
    sout.set_context(infinit::model::doughnut::OKBDontWaitForSignature{});
    sout.serialize_forward(b);
  }
  elle::IOStream is(buf.istreambuf());
  elle::serialization::binary::SerializerIn sin(is, false);
  sin.set_context<infinit::model::Model*>(&dht); // FIXME: needed ?
  sin.set_context<infinit::model::doughnut::Doughnut*>(&dht);
  sin.set_context(infinit::model::doughnut::ACBDontWaitForSignature{});
  sin.set_context(infinit::model::doughnut::OKBDontWaitForSignature{});
  auto res = sin.deserialize<std::unique_ptr<blocks::Block>>();
  res->seal();
  return res;
}

ELLE_TEST_SCHEDULED(serialize, (bool, paxos))
{ // test serialization used by async
  DHTs dhts(paxos);
  {
    auto b =  dhts.dht_a->make_block<blocks::ACLBlock>();
    b->data(elle::Buffer("foo"));
    b->seal();
    auto addr = b->address();
    auto cb = cycle(*dhts.dht_a, std::move(b));
    dhts.dht_a->insert(std::move(cb));
    cb = dhts.dht_a->fetch(addr);
    BOOST_CHECK_EQUAL(cb->data(), elle::Buffer("foo"));
  }
  { // wait for signature
    auto b =  dhts.dht_a->make_block<blocks::ACLBlock>();
    b->data(elle::Buffer("foo"));
    b->seal();
    elle::reactor::sleep(100_ms);
    auto addr = b->address();
    auto cb = cycle(*dhts.dht_a, std::move(b));
    dhts.dht_a->insert(std::move(cb));
    cb = dhts.dht_a->fetch(addr);
    BOOST_CHECK_EQUAL(cb->data(), elle::Buffer("foo"));
  }
  { // block we dont own
    auto block_alice = dhts.dht_a->make_block<blocks::ACLBlock>();
    block_alice->data(elle::Buffer("alice_1"));
    block_alice->set_permissions(dht::User(dhts.keys_b->K(), "bob"),
                                 true, true);
    auto addr = block_alice->address();
    dhts.dht_a->insert(std::move(block_alice));
    auto block_bob = dhts.dht_b->fetch(addr);
    BOOST_CHECK_EQUAL(block_bob->data(), elle::Buffer("alice_1"));
    dynamic_cast<blocks::MutableBlock*>(block_bob.get())->data(
      elle::Buffer("bob_1"));
    block_bob->seal();
    block_bob = cycle(*dhts.dht_b, std::move(block_bob));
    dhts.dht_b->update(std::move(block_bob));
    block_bob = dhts.dht_a->fetch(addr);
    BOOST_CHECK_EQUAL(block_bob->data(), elle::Buffer("bob_1"));
  }
  { // signing with group key
    std::unique_ptr<elle::cryptography::rsa::PublicKey> gkey;
    {
      infinit::model::doughnut::Group g(*dhts.dht_a, "g");
      g.create();
      g.add_member(dht::User(dhts.keys_b->K(), "bob"));
      gkey.reset(new elle::cryptography::rsa::PublicKey(
                   g.public_control_key()));
    }
    auto block_alice = dhts.dht_a->make_block<blocks::ACLBlock>();
    block_alice->data(elle::Buffer("alice_1"));
    block_alice->set_permissions(dht::User(*gkey, "@g"), true, true);
    auto addr = block_alice->address();
    dhts.dht_a->insert(std::move(block_alice));
    auto block_bob = dhts.dht_b->fetch(addr);
    BOOST_CHECK_EQUAL(block_bob->data(), elle::Buffer("alice_1"));
    dynamic_cast<blocks::MutableBlock*>(block_bob.get())->data(
      elle::Buffer("bob_1"));
    block_bob->seal();
    block_bob = cycle(*dhts.dht_b, std::move(block_bob));
    dhts.dht_b->update(std::move(block_bob));
    block_bob = dhts.dht_a->fetch(addr);
    BOOST_CHECK_EQUAL(block_bob->data(), elle::Buffer("bob_1"));
  }
}

#ifndef INFINIT_WINDOWS
ELLE_TEST_SCHEDULED(monitoring, (bool, paxos))
{
  auto keys_a = elle::cryptography::rsa::keypair::generate(key_size());
  auto keys_b = elle::cryptography::rsa::keypair::generate(key_size());
  auto keys_c = elle::cryptography::rsa::keypair::generate(key_size());
  auto id_a = infinit::model::Address::random(0); // FIXME
  auto id_b = infinit::model::Address::random(0); // FIXME
  auto id_c = infinit::model::Address::random(0); // FIXME
  Memory::Blocks storage_a;
  Memory::Blocks storage_b;
  Memory::Blocks storage_c;
  elle::filesystem::TemporaryDirectory d;
  auto monitoring_path = d.path() / "monitoring.sock";
  DHTs dhts(
    paxos,
    keys_a,
    keys_b,
    keys_c,
    id_a,
    id_b,
    id_c,
    std::make_unique<Memory>(storage_a),
    std::make_unique<Memory>(storage_b),
    std::make_unique<Memory>(storage_c),
    monitoring_socket_path_a = monitoring_path
  );
  BOOST_CHECK(boost::filesystem::exists(monitoring_path));
  elle::reactor::network::UnixDomainSocket socket(monitoring_path);
  using Monitoring = infinit::model::MonitoringServer;
  using Query = infinit::model::MonitoringServer::MonitorQuery::Query;
  auto do_query = [&] (Query query_val) -> elle::json::Object
    {
      auto query = Monitoring::MonitorQuery(query_val);
      elle::serialization::json::serialize(query, socket, false, false);
      return boost::any_cast<elle::json::Object>(elle::json::read(socket));
    };
  {
    Monitoring::MonitorResponse res(do_query(Query::Status));
    BOOST_CHECK(res.success);
  }
  {
    Monitoring::MonitorResponse res(do_query(Query::Stats));
    auto obj = res.result.get();
    BOOST_CHECK_EQUAL(obj.count("consensus"), 1);
    BOOST_CHECK_EQUAL(obj.count("overlay"), 1);
    BOOST_CHECK_EQUAL(boost::any_cast<std::string>(obj["protocol"]), "all");
    BOOST_CHECK_EQUAL(
      boost::any_cast<elle::json::Array>(obj["peers"]).size(), 3);
    auto redundancy = boost::any_cast<elle::json::Object>(obj["redundancy"]);
    if (paxos)
    {
      BOOST_CHECK_EQUAL(boost::any_cast<int64_t>(redundancy["desired_factor"]),
                        3);
      BOOST_CHECK_EQUAL(boost::any_cast<std::string>(redundancy["type"]),
                        "replication");
    }
    else
    {
      BOOST_CHECK_EQUAL(boost::any_cast<int64_t>(redundancy["desired_factor"]),
                        1);
      BOOST_CHECK_EQUAL(boost::any_cast<std::string>(redundancy["type"]),
                        "none");
    }
  }
}
#endif

template <typename T>
int
size(elle::reactor::Generator<T> const& g)
{
  int res = 0;
  for (auto const& m: elle::unconst(g))
  {
    (void)m;
    ++res;
  }
  return res;
};

namespace rebalancing
{
  ELLE_TEST_SCHEDULED(extend_and_write)
  {
    auto dht_a = DHT(dht::consensus::rebalance_auto_expand = false);
    ELLE_LOG("first DHT: %s", dht_a.dht->id());
    auto dht_b = DHT(dht::consensus::rebalance_auto_expand = false);
    ELLE_LOG("second DHT: %s", dht_b.dht->id());
    auto b1 = dht_a.dht->make_block<blocks::MutableBlock>();
    ELLE_LOG("write block to quorum of 1")
    {
      b1->data(std::string("extend_and_write 1"));
      dht_a.dht->seal_and_insert(*b1);
    }
    dht_b.overlay->connect(*dht_a.overlay);
    BOOST_CHECK_EQUAL(size(dht_a.overlay->lookup(b1->address(), 3)), 1u);
    BOOST_CHECK_EQUAL(size(dht_b.overlay->lookup(b1->address(), 3)), 1u);
    auto& paxos_a =
      dynamic_cast<dht::consensus::Paxos&>(*dht_a.dht->consensus());
    ELLE_LOG("rebalance block to quorum of 2")
      paxos_a.rebalance(b1->address());
    ELLE_LOG("write block to quorum of 2")
    {
      b1->data(std::string("extend_and_write 1 bis"));
      dht_a.dht->seal_and_update(*b1);
    }
    BOOST_CHECK_EQUAL(size(dht_a.overlay->lookup(b1->address(), 3)), 2u);
    BOOST_CHECK_EQUAL(size(dht_b.overlay->lookup(b1->address(), 3)), 2u);
  }

  template<typename BT>
  void
  run_extend_shrink_and_write()
  {
    DHT dht_a(dht::consensus::rebalance_auto_expand = true,
      dht::consensus::node_timeout = std::chrono::seconds(0));
    ELLE_LOG("first DHT: %s", dht_a.dht->id());
    DHT dht_b(dht::consensus::rebalance_auto_expand = true,
      dht::consensus::node_timeout = std::chrono::seconds(0));
    DHT dht_c(dht::consensus::rebalance_auto_expand = false);
    ELLE_LOG("third DHT: %s", dht_b.dht->id());
    dht_b.overlay->connect(*dht_a.overlay);
    dht_c.overlay->connect(*dht_a.overlay);
    dht_b.overlay->connect(*dht_c.overlay);
    auto b1 = dht_a.dht->make_block<BT>();
    ELLE_LOG("write block to quorum of 3")
    {
      b1->data(std::string("extend_and_write 1"));
      dht_a.dht->seal_and_insert(*b1);
    }
    BOOST_CHECK_EQUAL(size(dht_a.overlay->lookup(b1->address(), 3)), 3u);
    BOOST_CHECK_EQUAL(size(dht_b.overlay->lookup(b1->address(), 3)), 3u);
    dht_c.overlay->disconnect(*dht_a.overlay);
    dht_c.overlay->disconnect(*dht_b.overlay);
    auto& paxos_a =
      dynamic_cast<dht::consensus::Paxos&>(*dht_a.dht->consensus());
    ELLE_LOG("rebalance block to quorum of 2")
      paxos_a.rebalance(b1->address());
      DHT dht_d(dht::consensus::rebalance_auto_expand = false);
    BOOST_CHECK_EQUAL(size(dht_a.overlay->lookup(b1->address(), 3)), 2u);
    BOOST_CHECK_EQUAL(size(dht_b.overlay->lookup(b1->address(), 3)), 2u);
    dht_d.overlay->connect(*dht_a.overlay);
    dht_d.overlay->connect(*dht_b.overlay);
    ELLE_LOG("rebalance block to quorum of 3")
      paxos_a.rebalance(b1->address());
    ELLE_LOG("write block to quorum of 3")
    {
      b1->data(std::string("extend_and_write 1 bis"));
      try
      {
        dht_a.dht->seal_and_update(*b1);
      }
      catch (infinit::model::Conflict const& c)
      {
        ELLE_LOG("second write attempt");
        dynamic_cast<infinit::model::blocks::MutableBlock&>(*c.current())
          .data(std::string("extend_and_write 1 bis"));
        try
        {
          dht_a.dht->seal_and_update(*c.current());
        }
        catch (infinit::model::Conflict const& c)
        {
          ELLE_LOG("third write attempt");
          dynamic_cast<infinit::model::blocks::MutableBlock&>(*c.current())
            .data(std::string("extend_and_write 1 bis"));
          dht_a.dht->seal_and_update(*c.current());
        }
      }
    }
    BOOST_CHECK_EQUAL(size(dht_a.overlay->lookup(b1->address(), 3)), 3u);
    BOOST_CHECK_EQUAL(size(dht_b.overlay->lookup(b1->address(), 3)), 3u);
  }

  ELLE_TEST_SCHEDULED(extend_shrink_and_write)
  {
    run_extend_shrink_and_write<infinit::model::blocks::MutableBlock>();
    run_extend_shrink_and_write<infinit::model::blocks::ACLBlock>();
  }

  ELLE_TEST_SCHEDULED(shrink_and_write)
  {
    auto dht_a = DHT(dht::consensus::rebalance_auto_expand = false);
    ELLE_LOG("first DHT: %s", dht_a.dht->id());
    auto dht_b = DHT(dht::consensus::rebalance_auto_expand = false);
    ELLE_LOG("second DHT: %s", dht_b.dht->id());
    dht_b.overlay->connect(*dht_a.overlay);
    auto b1 = dht_a.dht->make_block<blocks::MutableBlock>();
    ELLE_LOG("write block to quorum of 2")
    {
      b1->data(std::string("shrink_kill_and_write 1"));
      dht_a.dht->seal_and_insert(*b1);
    }
    auto& paxos_a =
      dynamic_cast<dht::consensus::Paxos&>(*dht_a.dht->consensus());
    ELLE_LOG("rebalance block to quorum of 1")
      paxos_a.rebalance(b1->address(), {dht_a.dht->id()});
    ELLE_LOG("write block to quorum of 1")
    {
      b1->data(std::string("extend_and_write 2"));
      dht_a.dht->seal_and_update(*b1);
    }
  }

  ELLE_TEST_SCHEDULED(shrink_kill_and_write)
  {
    auto dht_a = DHT(dht::consensus::rebalance_auto_expand = false);
    ELLE_LOG("first DHT: %s", dht_a.dht->id());
    auto dht_b = DHT(dht::consensus::rebalance_auto_expand = false);
    ELLE_LOG("second DHT: %s", dht_b.dht->id());
    dht_b.overlay->connect(*dht_a.overlay);
    auto b1 = dht_a.dht->make_block<blocks::MutableBlock>();
    ELLE_LOG("write block to quorum of 2")
    {
      b1->data(std::string("shrink_kill_and_write 1"));
      dht_a.dht->seal_and_insert(*b1);
    }
    auto& paxos_a =
      dynamic_cast<dht::consensus::Paxos&>(*dht_a.dht->consensus());
    ELLE_LOG("rebalance block to quorum of 1")
      paxos_a.rebalance(b1->address(), {dht_a.dht->id()});
    dht_b.overlay->disconnect(*dht_a.overlay);
    ELLE_LOG("write block to quorum of 1")
    {
      b1->data(std::string("extend_and_write 2"));
      dht_a.dht->seal_and_update(*b1);
    }
  }

  ELLE_TEST_SCHEDULED(quorum_duel_1)
  {
    auto dht_a = DHT(dht::consensus::rebalance_auto_expand = false);
    ELLE_LOG("first DHT: %f", dht_a.dht->id());
    auto dht_b = DHT(dht::consensus::rebalance_auto_expand = false);
    ELLE_LOG("second DHT: %f", dht_b.dht->id());
    dht_b.overlay->connect_recursive(*dht_a.overlay);
    auto dht_c = DHT(dht::consensus::rebalance_auto_expand = false);
    ELLE_LOG("third DHT: %f", dht_c.dht->id());
    dht_c.overlay->connect_recursive(*dht_a.overlay);
    auto b = dht_a.dht->make_block<blocks::MutableBlock>();
    ELLE_LOG("write block to quorum of 3")
    {
      b->data(std::string("quorum_duel"));
      dht_a.dht->seal_and_insert(*b);
    }
    BOOST_CHECK_EQUAL(mutable_block_count(dht_c.overlay->blocks()), 1u);
    ELLE_LOG("disconnect third DHT")
      dht_c.overlay->disconnect_all();
    ELLE_LOG("rebalance block to quorum of 1")
    {
      auto& local_a =
        dynamic_cast<dht::consensus::Paxos&>(*dht_a.dht->consensus());
      local_a.rebalance(b->address(), {dht_a.dht->id()});
    }
    ELLE_LOG("reconnect third DHT")
      dht_c.overlay->connect_recursive(*dht_a.overlay);
    ELLE_LOG("write block to quorum of 1")
    {
      BOOST_CHECK_EQUAL(mutable_block_count(dht_c.overlay->blocks()), 1u);
      b->data(std::string("quorum_duel_edited"));
      dht_c.dht->seal_and_update(*b);
    }
  }

  ELLE_TEST_SCHEDULED(quorum_duel_2)
  {
    auto dht_a = DHT(dht::consensus::rebalance_auto_expand = false);
    ELLE_LOG("first DHT: %f", dht_a.dht->id());
    auto dht_b = DHT(dht::consensus::rebalance_auto_expand = false);
    ELLE_LOG("second DHT: %f", dht_b.dht->id());
    dht_b.overlay->connect_recursive(*dht_a.overlay);
    auto dht_c = DHT(dht::consensus::rebalance_auto_expand = false);
    ELLE_LOG("third DHT: %f", dht_c.dht->id());
    dht_c.overlay->connect_recursive(*dht_a.overlay);
    auto b = dht_a.dht->make_block<blocks::MutableBlock>();
    ELLE_LOG("write block to quorum of 3")
    {
      b->data(std::string("quorum_duel"));
      dht_a.dht->seal_and_insert(*b);
    }
    BOOST_CHECK_EQUAL(mutable_block_count(dht_c.overlay->blocks()), 1u);
    ELLE_LOG("disconnect third DHT")
      dht_c.overlay->disconnect_all();
    ELLE_LOG("rebalance block to quorum of 2")
    {
      auto& local_a =
        dynamic_cast<dht::consensus::Paxos&>(*dht_a.dht->consensus());
      local_a.rebalance(b->address(), {dht_a.dht->id(), dht_c.dht->id()});
    }
    ELLE_LOG("reconnect third DHT")
      dht_c.overlay->connect_recursive(*dht_a.overlay);
    ELLE_LOG("write block to quorum of 2")
    {
      BOOST_CHECK_EQUAL(mutable_block_count(dht_c.overlay->blocks()), 1u);
      b->data(std::string("quorum_duel_edited"));
      dht_c.dht->seal_and_update(*b);
    }
  }

  class VersionHop:
    public infinit::model::ConflictResolver
  {
  public:
    VersionHop(blocks::Block& previous)
      : _previous(previous.data())
    {}

    std::unique_ptr<blocks::Block>
    operator () (blocks::Block& failed,
                 blocks::Block& current) override
    {
      BOOST_CHECK_EQUAL(current.data(), this->_previous);
      return failed.clone();
    }

    void
    serialize(elle::serialization::Serializer& s,
              elle::Version const&) override
    {
      s.serialize("previous", this->_previous);
    }

    std::string
    description() const override
    {
      return "";
    }

    ELLE_ATTRIBUTE_R(elle::Buffer, previous);
  };

  class Local:
    public dht::consensus::Paxos::LocalPeer
  {
  public:
    using Super = dht::consensus::Paxos::LocalPeer;
    using Address = infinit::model::Address;

    template <typename ... Args>
    Local(infinit::model::doughnut::consensus::Paxos& paxos,
          int factor,
          bool rebalance_auto_expand,
          std::chrono::system_clock::duration node_timeout,
          infinit::model::doughnut::Doughnut& dht,
          Address id,
          Args&& ... args)
      : infinit::model::doughnut::Peer(dht, id)
      , Super(paxos,
              factor,
              rebalance_auto_expand,
              true,
              node_timeout,
              dht,
              id,
              std::forward<Args>(args)...)
      , _all_barrier()
      , _propose_barrier()
      , _propose_bypass(false)
      , _accept_barrier()
      , _accept_bypass(false)
      , _confirm_barrier()
      , _confirm_bypass(false)
    {
      this->_all_barrier.open();
      this->_propose_barrier.open();
      this->_accept_barrier.open();
      this->_confirm_barrier.open();
    }

    boost::optional<PaxosClient::Accepted>
    propose(PaxosServer::Quorum const& peers,
            Address address,
            PaxosClient::Proposal const& p) override
    {
      this->_proposing(address);
      elle::reactor::wait(this->all_barrier());
      if (!this->_propose_bypass)
        elle::reactor::wait(this->propose_barrier());
      auto res = Super::propose(peers, address, p);
      this->_proposed(address);
      return res;
    }

    PaxosClient::Proposal
    accept(PaxosServer::Quorum const& peers,
           Address address,
           PaxosClient::Proposal const& p,
           Value const& value) override
    {
      elle::reactor::wait(this->all_barrier());
      if (!this->_accept_bypass)
        elle::reactor::wait(this->accept_barrier());
      return Super::accept(peers, address, p, value);
    }

    void
    confirm(PaxosServer::Quorum const& peers,
            Address address,
            PaxosClient::Proposal const& p) override
    {
      elle::reactor::wait(this->all_barrier());
      if (!this->_confirm_bypass)
        elle::reactor::wait(this->confirm_barrier());
      Super::confirm(peers, address, p);
    }

    void
    _disappeared_schedule_eviction(infinit::model::Address id) override
    {
      ELLE_TRACE("%s: node %f disappeared, evict when signaled", this, id);
      this->_evict.connect([this, id] { this->_disappeared_evict(id); });
    }

    ELLE_ATTRIBUTE_RX(elle::reactor::Barrier, all_barrier);
    ELLE_ATTRIBUTE_RX(elle::reactor::Barrier, propose_barrier);
    ELLE_ATTRIBUTE_RW(bool, propose_bypass);
    ELLE_ATTRIBUTE_RX(boost::signals2::signal<void(Address)>, proposing);
    ELLE_ATTRIBUTE_RX(boost::signals2::signal<void(Address)>, proposed);
    ELLE_ATTRIBUTE_RX(elle::reactor::Barrier, accept_barrier);
    ELLE_ATTRIBUTE_RW(bool, accept_bypass);
    ELLE_ATTRIBUTE_RX(elle::reactor::Barrier, confirm_barrier);
    ELLE_ATTRIBUTE_RW(bool, confirm_bypass);
    ELLE_ATTRIBUTE_RX(boost::signals2::signal<void()>, evict);
  };

  static constexpr
  auto default_node_timeout = std::chrono::seconds(1);

  class InstrumentedPaxos
    : public dht::consensus::Paxos
  {
    using Super = dht::consensus::Paxos;
    using Super::Super;
    std::unique_ptr<dht::Local>
    make_local(
      boost::optional<int> port,
      boost::optional<boost::asio::ip::address> listen,
      std::unique_ptr<infinit::storage::Storage> storage,
      dht::Protocol p) override
    {
      return std::make_unique<Local>(
        *this,
        this->factor(),
        this->rebalance_auto_expand(),
        std::chrono::duration_cast<std::chrono::system_clock::duration>(
          default_node_timeout),
        this->doughnut(),
        this->doughnut().id(),
        std::move(storage),
        port.value_or(0));
    }
  };

  std::function<std::unique_ptr<dht::consensus::Consensus>(
    std::unique_ptr<dht::consensus::Consensus>)>
  instrument(int factor)
  {
    return [factor] (std::unique_ptr<dht::consensus::Consensus> c)
      -> std::unique_ptr<dht::consensus::Consensus>
    {
      return std::make_unique<InstrumentedPaxos>(
        dht::consensus::doughnut = c->doughnut(),
        dht::consensus::replication_factor = factor);
    };
  }

  static
  std::unique_ptr<blocks::Block>
  make_block(DHT& client, bool immutable, std::string data_)
  {
    auto data = elle::Buffer(std::move(data_));
    if (immutable)
      return client.dht->make_block<blocks::ImmutableBlock>(std::move(data));
    else
    {
      auto b = client.dht->make_block<blocks::MutableBlock>();
      b->data(std::move(data));
      return std::move(b);
    }
  }

  ELLE_TEST_SCHEDULED(expand_new_block, (bool, immutable))
  {
    auto dht_a = DHT(make_consensus = instrument(2));
    auto& local_a = dynamic_cast<Local&>(*dht_a.dht->local());
    ELLE_LOG("first DHT: %s", dht_a.dht->id());
    auto dht_b = DHT(make_consensus = instrument(2));
    dht_b.overlay->connect(*dht_a.overlay);
    ELLE_LOG("second DHT: %s", dht_b.dht->id());
    auto client = DHT(storage = nullptr);
    client.overlay->connect(*dht_a.overlay);
    auto b = make_block(client, immutable, "expand_new_block");
    ELLE_LOG("write block to one DHT")
      client.dht->seal_and_insert(*b);
    BOOST_CHECK_EQUAL(size(dht_a.overlay->lookup(b->address(), 2)), 1u);
    BOOST_CHECK_EQUAL(size(dht_b.overlay->lookup(b->address(), 2)), 1u);
    ELLE_LOG("wait for rebalancing")
      elle::reactor::wait(local_a.rebalanced(), b->address());
    BOOST_CHECK_EQUAL(size(dht_a.overlay->lookup(b->address(), 2)), 2u);
    BOOST_CHECK_EQUAL(size(dht_b.overlay->lookup(b->address(), 2)), 2u);
    ELLE_LOG("disconnect second DHT")
      dht_b.overlay->disconnect(*dht_a.overlay);
    ELLE_LOG("read block from second DHT")
      BOOST_CHECK_EQUAL(dht_b.dht->fetch(b->address())->data(), b->data());
  }

  ELLE_TEST_SCHEDULED(expand_newcomer, (bool, immutable))
  {
    auto dht_a = DHT(make_consensus = instrument(3));
    auto& local_a = dynamic_cast<Local&>(*dht_a.dht->local());
    ELLE_LOG("first DHT: %s", dht_a.dht->id());
    auto dht_b = DHT(make_consensus = instrument(3));
    ELLE_LOG("second DHT: %s", dht_b.dht->id());
    auto b = make_block(dht_a, immutable, "expand_newcomer");
    ELLE_LOG("write block to first DHT")
      dht_a.dht->seal_and_insert(*b);
    // Block the new quorum election to check the balancing is done in
    // background.
    local_a.propose_barrier().close();
    // Wait until the first automatic expansion fails.
    elle::reactor::wait(dht_a.overlay->looked_up(), b->address());
    ELLE_LOG("connect second DHT")
      dht_b.overlay->connect(*dht_a.overlay);
    if (!immutable)
    {
      elle::reactor::wait(local_a.proposing(), b->address());
      BOOST_CHECK_EQUAL(size(dht_a.overlay->lookup(b->address(), 3)), 1u);
      BOOST_CHECK_EQUAL(size(dht_b.overlay->lookup(b->address(), 3)), 1u);
      // Insert another block, to check iterator invalidation while balancing.
      ELLE_LOG("write other block to first DHT")
      {
        local_a.propose_bypass(true);
        auto perturbate = dht_a.dht->make_block<blocks::MutableBlock>();
      perturbate->data(std::string("booh!"));
      dht_a.dht->seal_and_insert(*perturbate);
      }
      local_a.propose_barrier().open();
    }
    ELLE_LOG("wait for rebalancing")
      elle::reactor::wait(local_a.rebalanced(), b->address());
    BOOST_CHECK_EQUAL(size(dht_a.overlay->lookup(b->address(), 3)), 2u);
    BOOST_CHECK_EQUAL(size(dht_b.overlay->lookup(b->address(), 3)), 2u);
    if (!immutable)
    {
      auto& mb = dynamic_cast<blocks::MutableBlock&>(*b);
      ELLE_LOG("write block to both DHTs")
      {
        auto resolver = std::make_unique<VersionHop>(mb);
        mb.data(std::string("expand'"));
        dht_b.dht->seal_and_update(mb, std::move(resolver));
      }
    }
    ELLE_LOG("disconnect second DHT")
      dht_b.overlay->disconnect(*dht_a.overlay);
    ELLE_LOG("read block from second DHT")
      BOOST_CHECK_EQUAL(dht_b.dht->fetch(b->address())->data(), b->data());
  }

  ELLE_TEST_SCHEDULED(expand_concurrent)
  {
    auto dht_a = DHT(make_consensus = instrument(3));
    auto& local_a = dynamic_cast<Local&>(*dht_a.dht->local());
    ELLE_LOG("first DHT: %s", dht_a.dht->id());
    auto dht_b = DHT(make_consensus = instrument(3));
    auto& local_b = dynamic_cast<Local&>(*dht_b.dht->local());
    dht_b.overlay->connect(*dht_a.overlay);
    ELLE_LOG("second DHT: %s", dht_b.dht->id());
    auto dht_c = DHT(make_consensus = instrument(3));
    dht_c.overlay->connect(*dht_a.overlay);
    dht_c.overlay->connect(*dht_b.overlay);
    ELLE_LOG("third DHT: %s", dht_b.dht->id());
    auto client = DHT(storage = nullptr);
    client.overlay->connect(*dht_a.overlay);
    client.overlay->connect(*dht_b.overlay);
    auto b = client.dht->make_block<blocks::MutableBlock>();
    ELLE_LOG("write block to two DHT")
    {
      b->data(std::string("expand"));
      client.dht->seal_and_insert(*b);
    }
    ELLE_LOG("wait for rebalancing")
    {
      boost::signals2::signal<void(infinit::model::Address)> rebalanced;
      boost::signals2::scoped_connection c_a =
        local_a.rebalanced().connect(rebalanced);
      boost::signals2::scoped_connection c_b =
        local_b.rebalanced().connect(rebalanced);
      elle::reactor::wait(rebalanced, b->address());
    }
    BOOST_CHECK_EQUAL(size(dht_a.overlay->lookup(b->address(), 3)), 3u);
    BOOST_CHECK_EQUAL(size(dht_b.overlay->lookup(b->address(), 3)), 3u);
    BOOST_CHECK_EQUAL(size(dht_c.overlay->lookup(b->address(), 3)), 3u);
  }

  ELLE_TEST_SCHEDULED(expand_from_disk, (bool, immutable))
  {
    infinit::storage::Memory::Blocks storage_a;
    infinit::model::Address address;
    auto id_a = infinit::model::Address::random();
    ELLE_LOG("create block with 1 DHT")
    {
      auto dht_a = DHT(id = id_a,
                make_consensus = instrument(3),
                storage = std::make_unique<Memory>(storage_a));
      auto block = make_block(dht_a, immutable, "expand_from_disk");
      address = block->address();
      dht_a.dht->insert(std::move(block));
    }
    BOOST_CHECK(storage_a.find(address) != storage_a.end());
    ELLE_LOG("restart with 2 DHTs")
    {
      auto dht_a = DHT(id = id_a,
                make_consensus = instrument(3),
                storage = std::make_unique<Memory>(storage_a));
      auto& local_a = dynamic_cast<Local&>(*dht_a.dht->local());
      auto dht_b = DHT(make_consensus = instrument(3));
      dht_b.overlay->connect(*dht_a.overlay);
      elle::reactor::wait(local_a.rebalanced(), address);
    }
  }

  ELLE_TEST_SCHEDULED(rebalancing_while_destroyed)
  {
    DHT dht_a;
    ELLE_LOG("first DHT: %s", dht_a.dht->id());
    DHT dht_b;
    ELLE_LOG("second DHT: %s", dht_b.dht->id());
    auto b1 = dht_a.dht->make_block<blocks::MutableBlock>();
    ELLE_LOG("write block to quorum of 1")
    {
      b1->data(std::string("extend_and_write 1"));
      dht_a.dht->seal_and_insert(*b1);
    }
    dht_b.overlay->connect(*dht_a.overlay);
  }

  ELLE_TEST_SCHEDULED(evict_faulty, (bool, immutable))
  {
    auto dht_a = DHT(make_consensus = instrument(3));
    auto& local_a = dynamic_cast<Local&>(*dht_a.dht->local());
    ELLE_LOG("first DHT: %f", dht_a.dht->id());
    auto dht_b = DHT(make_consensus = instrument(3));
    auto& local_b = dynamic_cast<Local&>(*dht_b.dht->local());
    dht_b.overlay->connect(*dht_a.overlay);
    ELLE_LOG("second DHT: %f", dht_b.dht->id());
    auto dht_c = DHT(make_consensus = instrument(3));
    dht_c.overlay->connect(*dht_a.overlay);
    dht_c.overlay->connect(*dht_b.overlay);
    ELLE_LOG("third DHT: %f", dht_c.dht->id());
    auto b = make_block(dht_a, immutable, "evict_faulty");
    ELLE_LOG("write block")
      dht_a.dht->seal_and_insert(*b);
    auto dht_d = DHT(make_consensus = instrument(3));
    dht_d.overlay->connect(*dht_a.overlay);
    dht_d.overlay->connect(*dht_b.overlay);
    dht_d.overlay->connect(*dht_c.overlay);
    ELLE_LOG("fourth DHT: %f", dht_d.dht->id());
    ELLE_LOG("disconnect third DHT")
    {
      dht_c.overlay->disconnect_all();
      local_a.evict()();
      local_b.evict()();
    }
    ELLE_LOG("wait for rebalancing")
    {
      boost::signals2::signal<void(infinit::model::Address)> rebalanced;
      boost::signals2::scoped_connection c_a =
        local_a.rebalanced().connect(rebalanced);
      boost::signals2::scoped_connection c_b =
        local_b.rebalanced().connect(rebalanced);
      elle::reactor::wait(rebalanced, b->address());
    }
    ELLE_LOG("disconnect first DHT")
      dht_a.overlay->disconnect_all();
    ELLE_LOG("read block")
      BOOST_CHECK_EQUAL(dht_b.dht->fetch(b->address())->data(), b->data());
  }
}

// Since we use Locals, blocks dont go through serialization and thus
// are fetched already decoded
static void no_cheating(dht::Doughnut* d, std::unique_ptr<blocks::Block>& b)
{
  auto acb = dynamic_cast<dht::ACB*>(b.get());
  if (!acb)
    return;
  elle::Buffer buf;
  {
    elle::IOStream os(buf.ostreambuf());
    elle::serialization::binary::serialize(b, os);
  }
  elle::IOStream is(buf.istreambuf());
  elle::serialization::Context ctx;
  ctx.set(d);
  auto res =
    elle::serialization::binary::deserialize<std::unique_ptr<blocks::Block>>(
      is, true, ctx);
  b.reset(res.release());
}

ELLE_TEST_SCHEDULED(batch_quorum)
{
  auto owner_key = elle::cryptography::rsa::keypair::generate(512);
  auto dht_a = DHT(keys=owner_key, owner=owner_key);
  auto dht_b = DHT(keys=owner_key, owner=owner_key);
  auto dht_c = DHT(keys=owner_key, owner=owner_key);
  dht_b.overlay->connect(*dht_a.overlay);
  dht_c.overlay->connect(*dht_a.overlay);
  dht_b.overlay->connect(*dht_c.overlay);
  std::vector<infinit::model::Model::AddressVersion> addrs;
  for (int i=0; i<10; ++i)
  {
    auto block = dht_a.dht->make_block<blocks::ACLBlock>();
    block->data(std::string("foo"));
    addrs.push_back(std::make_pair(block->address(), boost::optional<int>()));
    const_cast<Overlay&>(dynamic_cast<Overlay const&>(*dht_a.overlay)).
      partial_addresses()[block->address()] = 1 + (i % 3);
    dht_b.dht->seal_and_insert(*block);
  }
  int hit = 0;
  auto handler = [&](infinit::model::Address,
                     std::unique_ptr<blocks::Block> b,
                     std::exception_ptr ex)
    {
      if (ex)
      {
        try
        {
          std::rethrow_exception(ex);
        }
        catch (elle::Error const& e)
        {
          ELLE_ERR("boum %s", e);
        }
      }
      BOOST_CHECK(b);
      if (b)
        BOOST_CHECK_EQUAL(b->data(), std::string("foo"));
      if (b && !ex)
        ++hit;
    };
  dht_b.dht->multifetch(addrs, handler);
  BOOST_CHECK_EQUAL(hit, 10);
  hit = 0;
  dht_a.dht->multifetch(addrs, handler);
  BOOST_CHECK_EQUAL(hit, 10);
  dht_c.overlay->disconnect(*dht_a.overlay);
  dht_c.overlay->disconnect(*dht_b.overlay);
  hit = 0;
  dht_b.dht->multifetch(addrs, handler);
  BOOST_CHECK_EQUAL(hit, 10);
  hit = 0;
  dht_a.dht->multifetch(addrs, handler);
  BOOST_CHECK_EQUAL(hit, 10);
}

ELLE_TEST_SCHEDULED(admin_keys)
{
  auto owner_key = elle::cryptography::rsa::keypair::generate(512);
  auto dht = DHT(keys=owner_key, owner=owner_key);
  auto client = DHT(storage = nullptr, keys=owner_key, owner=owner_key);
  client.overlay->connect(*dht.overlay);
  auto b0 = client.dht->make_block<blocks::ACLBlock>();
  b0->data(std::string("foo"));
  client.dht->seal_and_insert(*b0);
  auto b1 = client.dht->make_block<blocks::ACLBlock>();
  b1->data(std::string("foo"));
  client.dht->seal_and_insert(*b1);
  auto b2 = client.dht->fetch(b1->address());
  no_cheating(client.dht.get(), b2);
  BOOST_CHECK_EQUAL(b2->data().string(), "foo");
  // set server-side adm key but don't tell the client
  auto admin = elle::cryptography::rsa::keypair::generate(512);
  dht.dht->admin_keys().r.push_back(admin.K());
  dynamic_cast<infinit::model::blocks::MutableBlock*>(b2.get())->
    data(std::string("bar"));
  BOOST_CHECK_THROW(client.dht->seal_and_update(*b2), std::exception);
  auto b3 = client.dht->make_block<blocks::ACLBlock>();
  b3->data(std::string("baz"));
  BOOST_CHECK_THROW(client.dht->seal_and_insert(*b3), std::exception);
  // tell the client
  client.dht->admin_keys().r.push_back(admin.K());
  b3 = client.dht->make_block<blocks::ACLBlock>();
  b3->data(std::string("baz"));
  BOOST_CHECK_NO_THROW(client.dht->seal_and_insert(*b3));

  // check admin can actually read the block
  auto cadm = DHT(storage = nullptr, keys=admin, owner=owner_key);
  cadm.dht->admin_keys().r.push_back(admin.K());
  cadm.overlay->connect(*dht.overlay);
  auto b4 = cadm.dht->fetch(b3->address());
  BOOST_CHECK_EQUAL(b4->data().string(), "baz");
  // but not the first one pushed before setting admin_key
  auto b0a = cadm.dht->fetch(b0->address());
  no_cheating(cadm.dht.get(), b0a);
  BOOST_CHECK_THROW(b0a->data(), std::exception);

  // do some stuff with blocks owned by admin
  auto ba = cadm.dht->make_block<blocks::ACLBlock>();
  ba->data(std::string("foo"));
  ba->set_permissions(*cadm.dht->make_user(elle::serialization::json::serialize(
    owner_key.K())), true, true);
  cadm.dht->seal_and_insert(*ba);
  auto ba2 = cadm.dht->fetch(ba->address());
  no_cheating(cadm.dht.get(), ba2);
  BOOST_CHECK_EQUAL(ba2->data(), std::string("foo"));
  auto ba3 = client.dht->fetch(ba->address());
  no_cheating(client.dht.get(), ba3);
  BOOST_CHECK_EQUAL(ba3->data(), std::string("foo"));
  dynamic_cast<infinit::model::blocks::MutableBlock*>(ba3.get())->data(
    std::string("bar"));
  client.dht->seal_and_update(*ba3);
  auto ba4 = cadm.dht->fetch(ba->address());
  no_cheating(cadm.dht.get(), ba4);
  BOOST_CHECK_EQUAL(ba4->data(), std::string("bar"));

  // try to change admin user's permissions
  auto b_perm = dht.dht->make_block<blocks::ACLBlock>();
  b_perm->data(std::string("admin user data"));
  dht.dht->seal_and_insert(*b_perm);
  auto fetched_b_perm = dht.dht->fetch(b_perm->address());
  no_cheating(dht.dht.get(), fetched_b_perm);
  b_perm.reset(dynamic_cast<blocks::ACLBlock*>(fetched_b_perm.release()));
  BOOST_CHECK_THROW(
    b_perm->set_permissions(
      *dht.dht->make_user(elle::serialization::json::serialize(admin.K())),
    false, false), elle::Error);

  // check group admin key
  auto gadmin = elle::cryptography::rsa::keypair::generate(512);
  auto cadmg = DHT(storage = nullptr, keys=gadmin, owner=owner_key);
  cadmg.overlay->connect(*dht.overlay);
  std::unique_ptr<elle::cryptography::rsa::PublicKey> g_K;
  {
    dht::Group g(*dht.dht, "g");
    g.create();
    g.add_member(*cadmg.dht->make_user(elle::serialization::json::serialize(
      gadmin.K())));
    cadmg.dht->admin_keys().group_r.push_back(g.public_control_key());
    dht.dht->admin_keys().group_r.push_back(g.public_control_key());
    client.dht->admin_keys().group_r.push_back(g.public_control_key());
    g_K.reset(
      new elle::cryptography::rsa::PublicKey(g.public_control_key()));
  }

  auto bg = client.dht->make_block<blocks::ACLBlock>();
  bg->data(std::string("baz"));
  client.dht->seal_and_insert(*bg);
  auto bg2 = cadmg.dht->fetch(bg->address());
  BOOST_CHECK_EQUAL(bg2->data(), std::string("baz"));

  // try to change admin group's permissions
  auto bg_perm = cadmg.dht->fetch(bg->address());
  // no_cheating(cadmg.dht.get(), bg_perm);
  BOOST_CHECK_THROW(
    dynamic_cast<blocks::ACLBlock*>(bg_perm.get())->set_permissions(
      *cadmg.dht->make_user(elle::serialization::json::serialize(g_K)),
    false, false), elle::Error);
}


ELLE_TEST_SCHEDULED(disabled_crypto)
{
  auto key = elle::cryptography::rsa::keypair::generate(key_size());
  infinit::model::doughnut::EncryptOptions eopts(false, false, false);
  DHTs dhts(true, encrypt_options = eopts, keys_a = key, keys_b=key, keys_c = key);
  auto b = dhts.dht_a->make_block<blocks::ACLBlock>(elle::Buffer("canard", 6));
  auto baddr = b->address();
  dhts.dht_a->insert(std::move(b));
  auto bc = dhts.dht_b->fetch(baddr);
  BOOST_CHECK_EQUAL(bc->data(), "canard");
  auto bi = dhts.dht_a->make_block<blocks::ImmutableBlock>(elle::Buffer("canard", 6));
  auto biaddr = bi->address();
  dhts.dht_a->insert(std::move(bi));
  auto bic = dhts.dht_b->fetch(biaddr);
  BOOST_CHECK_EQUAL(bic->data(), "canard");
}

ELLE_TEST_SUITE()
{
  auto& suite = boost::unit_test::framework::master_test_suite();
  boost::unit_test::test_suite* plain = BOOST_TEST_SUITE("plain");
  suite.add(plain);
  boost::unit_test::test_suite* paxos = BOOST_TEST_SUITE("paxos");
  suite.add(paxos);
#define TEST(Name)                              \
  {                                             \
    auto _Name = boost::bind(Name, true);       \
    auto Name = _Name;                          \
    paxos->add(BOOST_TEST_CASE(Name));          \
  }                                             \
  {                                             \
    auto _Name = boost::bind(Name, false);      \
    auto Name = _Name;                          \
    plain->add(BOOST_TEST_CASE(Name));          \
  }
  TEST(CHB);
  TEST(OKB);
  TEST(async);
  TEST(ACB);
  TEST(NB);
  TEST(UB);
  TEST(conflict);
  TEST(restart);
  TEST(cache);
  TEST(serialize);
#ifndef INFINIT_WINDOWS
  TEST(monitoring);
#endif
  {
    boost::unit_test::test_suite* remove_plain = BOOST_TEST_SUITE("removal");
    plain->add(remove_plain);
    boost::unit_test::test_suite* remove_paxos = BOOST_TEST_SUITE("removal");
    paxos->add(remove_paxos);
    auto* plain = remove_plain;
    auto* paxos = remove_paxos;
    using namespace removal;
    TEST(serialize_ACB_remove);
  }
#undef TEST
  paxos->add(BOOST_TEST_CASE(admin_keys));
  paxos->add(BOOST_TEST_CASE(batch_quorum));
  paxos->add(BOOST_TEST_CASE(wrong_quorum));
  paxos->add(BOOST_TEST_CASE(disabled_crypto));
  {
    using namespace tests_paxos;
    paxos->add(BOOST_TEST_CASE(CHB_no_peer));
  }
  {
    boost::unit_test::test_suite* rebalancing = BOOST_TEST_SUITE("rebalancing");
    paxos->add(rebalancing);
    using namespace rebalancing;
    rebalancing->add(BOOST_TEST_CASE(extend_and_write), 0, valgrind(3));
    rebalancing->add(BOOST_TEST_CASE(shrink_and_write), 0, valgrind(3));
    rebalancing->add(BOOST_TEST_CASE(extend_shrink_and_write), 0, valgrind(3));
    rebalancing->add(BOOST_TEST_CASE(shrink_kill_and_write), 0, valgrind(3));
    rebalancing->add(BOOST_TEST_CASE(quorum_duel_1), 0, valgrind(3));
    rebalancing->add(BOOST_TEST_CASE(quorum_duel_2), 0, valgrind(3));
    {
      auto expand_new_CHB = [] () { expand_new_block(true); };
      auto expand_new_OKB = [] () { expand_new_block(false); };
      rebalancing->add(BOOST_TEST_CASE(expand_new_CHB), 0, valgrind(3));
      rebalancing->add(BOOST_TEST_CASE(expand_new_OKB), 0, valgrind(3));
    }
    {
      auto expand_newcomer_CHB = [] () { expand_newcomer(true); };
      auto expand_newcomer_OKB = [] () { expand_newcomer(false); };
      rebalancing->add(BOOST_TEST_CASE(expand_newcomer_CHB), 0, valgrind(3));
      rebalancing->add(BOOST_TEST_CASE(expand_newcomer_OKB), 0, valgrind(3));
    }
    rebalancing->add(BOOST_TEST_CASE(expand_concurrent), 0, valgrind(5));
    {
      auto expand_CHB_from_disk = [] () { expand_from_disk(true); };
      auto expand_OKB_from_disk = [] () { expand_from_disk(false); };
      rebalancing->add(BOOST_TEST_CASE(expand_CHB_from_disk), 0, valgrind(3));
      rebalancing->add(BOOST_TEST_CASE(expand_OKB_from_disk), 0, valgrind(3));
    }
    rebalancing->add(
      BOOST_TEST_CASE(rebalancing_while_destroyed), 0, valgrind(3));
    {
      auto evict_faulty_CHB = [] () { evict_faulty(true); };
      auto evict_faulty_OKB = [] () { evict_faulty(false); };
      rebalancing->add(BOOST_TEST_CASE(evict_faulty_CHB), 0, valgrind(3));
      rebalancing->add(BOOST_TEST_CASE(evict_faulty_OKB), 0, valgrind(3));
    }
  }
}
