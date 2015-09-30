#include <infinit/model/doughnut/Replicator.hh>
#include <infinit/model/doughnut/Remote.hh>
#include <infinit/model/MissingBlock.hh>

#include <elle/serialization/Serializer.hh>
#include <elle/serialization/binary.hh>
#include <elle/serialization/binary/SerializerIn.hh>
#include <elle/serialization/binary/SerializerOut.hh>
#include <elle/serialization/json.hh>

#include <reactor/Channel.hh>
#include <reactor/Scope.hh>
#include <reactor/exception.hh>
#include <reactor/scheduler.hh>
#include <reactor/network/exception.hh>

#include <boost/filesystem/fstream.hpp>

ELLE_LOG_COMPONENT("infinit.model.doughnut.Replicator");

namespace elle
{
  namespace serialization
  {
    typedef std::chrono::system_clock::time_point Time;
    template<> struct Serialize<Time>
    {
      typedef uint64_t Type;
      static uint64_t convert(Time& t)
      {
        Type res = std::chrono::duration_cast<std::chrono::milliseconds>(
          t.time_since_epoch()).count();
        return res;
      }
      static Time convert(uint64_t repr)
      {
        return Time(std::chrono::milliseconds(repr));
      }
    };
  }
}

namespace infinit
{
  namespace model
  {
    namespace doughnut
    {
      Replicator::Replicator(Doughnut& doughnut, int factor,
                             boost::filesystem::path const& journal_dir)
      : Consensus(doughnut)
      , _factor(factor)
      , _overlay(nullptr)
      , _journal_dir(journal_dir)
      , _process_thread("replicator", [&] { _process_loop();})
      {
        ELLE_TRACE("%s: using journal at %s", *this, this->_journal_dir);
        boost::filesystem::create_directories(_journal_dir);
      }

      Replicator::~Replicator()
      {
        ELLE_TRACE_SCOPE("%s: destroy", *this);
        _process_thread.terminate_now();
      }

      void
      Replicator::_process_loop()
      {
        while (true)
        {
          try
          {
            _process_cache();
          }
          catch (reactor::Terminate const&)
          {
            ELLE_TRACE("Terminating thread");
            throw;
          }
          catch (std::exception const& e)
          {
            ELLE_WARN("Exception escaped replicator loop: %s", e.what());
          }
          reactor::sleep(10_sec);
        }
      }

      void
      Replicator::_store(overlay::Overlay& overlay,
                         blocks::Block& block, StoreMode mode)
      {
        ELLE_TRACE_SCOPE("%s: store %s", *this, block);
        _overlay = &overlay;
        overlay::Operation op;
        switch (mode)
        {
          case STORE_ANY:
            op = overlay::OP_INSERT_OR_UPDATE;
            break;
          case STORE_INSERT:
            op = overlay::OP_INSERT;
            break;
          case STORE_UPDATE:
            op = overlay::OP_UPDATE;
            break;
          default:
            elle::unreachable();
        }
        // Only allow create through if _factor nodes are reached.
        // Let other operations through with degraded node count.
        auto ipeers = overlay.lookup(block.address(), _factor, op);
        std::vector<overlay::Overlay::Member> peers;
        for (auto p: ipeers)
        {
          peers.push_back(p);
          if (signed(peers.size()) == _factor)
            break;
        }
        ELLE_DEBUG("overlay returned %s peers", peers.size());
        if (peers.empty() ||
             (op == overlay::OP_INSERT && signed(peers.size()) < _factor))
        {
          throw elle::Error(elle::sprintf("Got only %s of %s required peers",
                                          peers.size(), _factor));
        }
        elle::With<reactor::Scope>() <<  [&] (reactor::Scope& s)
        {
          for (auto p: peers)
          {
            s.run_background("store", [this, p, &block, mode]
            {
              for (int i=0; i<5; ++i)
              {
                try
                {
                  if (i!=0)
                    p->reconnect();
                  p->store(block, mode);
                  return;
                }
                catch (elle::Error const& e)
                {
                  ELLE_TRACE("%s: network exception %s", *this, e);
                  reactor::sleep(boost::posix_time::milliseconds(20*pow(2,i)));
                }
              }
              throw reactor::network::Exception(elle::sprintf(
                "%s: too many retries storing %s, aborting", *this, block.address()));
            });
          }
          reactor::wait(s);
        };
        std::string saddress = elle::sprintf("%s", block.address());
        if (peers.size() < unsigned(_factor))
        {
          ELLE_TRACE("store with only %s of %s nodes", peers.size(), _factor);
          boost::filesystem::ofstream ofs(_journal_dir / saddress);
          ofs << peers.size();
        }
        else
          boost::filesystem::remove(_journal_dir /saddress);
      }

      static
      std::unique_ptr<blocks::Block>
      fetch_from_members(reactor::Generator<overlay::Overlay::Member>& peers,
                         Address address)
      {
        std::unique_ptr<blocks::Block> result;
        reactor::Channel<overlay::Overlay::Member> connected;
        typedef reactor::Generator<overlay::Overlay::Member> PeerGenerator;
        // try connecting to all peers in parallel
        auto connected_peers = PeerGenerator([&](PeerGenerator::yielder yield) {
            elle::With<reactor::Scope>() <<  [&peers,&yield] (reactor::Scope& s)
            {
              for (auto p: peers)
              {
                s.run_background(elle::sprintf("connect to %s", *p),
                [p,&yield]
                {
                  for (int i=0; i<5; ++i)
                  {
                    ELLE_DEBUG_SCOPE("connect to %s", *p);
                    try
                    {
                      if (i!=0)
                        p->reconnect();
                      else
                        p->connect();
                      yield(p);
                      return;
                    }
                    catch (elle::Error const& e)
                    {
                      ELLE_TRACE("network exception %s", e);
                      reactor::sleep(boost::posix_time::milliseconds(20*pow(2,i)));
                    }
                  }
                });
              }
              reactor::wait(s);
            };
        });
        // try to get on all connected peers sequentially to avoid wasting bandwidth
        for (auto peer: connected_peers)
        {
          try
          {
            ELLE_TRACE_SCOPE("fetch from %s", *peer);
            return peer->fetch(address);
          }
          catch (elle::Error const& e)
          {
            ELLE_WARN("fetching from %s failed: %s", *peer, e.what());
          }
        }
        throw elle::Error(
            elle::sprintf("fetching of %s failed on all candidates", address));
      }

      std::unique_ptr<blocks::Block>
      Replicator::_fetch(overlay::Overlay& overlay, Address address)
      {
        ELLE_TRACE_SCOPE("%s: fetch %s", *this, address);
        this->_overlay = &overlay;
        auto peers = overlay.lookup(address, _factor, overlay::OP_FETCH);
        return fetch_from_members(peers, address);
      }

      void
      Replicator::_remove(overlay::Overlay& overlay, Address address)
      {
        _overlay = &overlay;
        auto peers = overlay.lookup(address, _factor, overlay::OP_REMOVE);
        int count = 0;
        for (auto const& p: peers)
        {
          p->remove(address);
          ++count;
        }
        if (!count)
          throw MissingBlock(address);
      }

      std::unique_ptr<blocks::Block>
      Replicator::_vote(overlay::Overlay::Members peers, Address address)
      {
        std::vector<std::unique_ptr<blocks::Block>> blocks;
        for (auto& peer: peers)
          blocks.push_back(peer->fetch(address));
        std::vector<std::unique_ptr<blocks::Block>> voted_blocks;
        std::vector<int> votes;
        for (auto& b: blocks)
        {
          auto it = std::find_if(voted_blocks.begin(), voted_blocks.end(),
            [&] (std::unique_ptr<blocks::Block>& vb) {
              return *b == *vb;
            });
          if (it == voted_blocks.end())
          {
            voted_blocks.push_back(std::move(b));
            votes.push_back(1);
          }
          else
            votes[it - voted_blocks.begin()]++;
        }
        auto maxIndex = std::max_element(votes.begin(), votes.end()) - votes.begin();
        int nmax = std::count(votes.begin(), votes.end(), votes[maxIndex]);
        if (nmax > 1)
          throw elle::Error(
            elle::sprintf("Multiple values with %s votes", votes[maxIndex]));
        return std::move(voted_blocks[maxIndex]);
      }

      void Replicator::_process_cache()
      {
        if (!_overlay)
          return;
        ELLE_TRACE("checking cache");
        boost::filesystem::directory_iterator it(_journal_dir);
        for (;it != boost::filesystem::directory_iterator(); ++it)
        {
          ELLE_TRACE("considering %s", it->path());
          Address address = Address::from_string(it->path().filename().string().substr(2));
          int known_replicas = 0;
          {
            boost::filesystem::ifstream ifs(it->path());
            ifs >> known_replicas;
          }
          overlay::Overlay::Members peers;
          try
          {
            for (auto p: _overlay->lookup(address, _factor, overlay::OP_FETCH))
            peers.push_back(p);
          }
          catch (MissingBlock const&)
          { // assume block was deleted
            ELLE_TRACE("no hit on %s, assuming removed block", address);
            boost::filesystem::remove(it->path());
            continue;
          }
          ELLE_TRACE("got %s/%s peers for %s",
                     peers.size(), known_replicas, address);
          if (signed(peers.size()) <=  known_replicas)
          {
            ELLE_TRACE("No new matches for %s", address);
            continue;
          }
          std::unique_ptr<blocks::Block> block;
          try
          {
            block = _vote(peers, address);
          }
          catch (elle::Error const& e)
          {
            ELLE_WARN("Failed to resolve block conflict");
            continue;
          }
          for (auto const& p: peers)
          {
            p->store(*block, STORE_UPDATE);
          }
          if (signed(peers.size()) == _factor)
          {
            boost::filesystem::remove(it->path());
          }
        }
        ELLE_TRACE("done checking cache");
      }
    }
  }
}
