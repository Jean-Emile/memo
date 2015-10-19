#include <infinit/model/doughnut/Async.hh>
#include <infinit/model/doughnut/ACB.hh>

#include <boost/filesystem.hpp>
#include <boost/filesystem/fstream.hpp>

#include <elle/serialization/json.hh>

#include <elle/serialization/binary.hh>

#include <reactor/exception.hh>
#include <reactor/scheduler.hh>
#include <infinit/model/MissingBlock.hh>

#include <iostream>

ELLE_LOG_COMPONENT("infinit.model.doughnut.Async");

namespace infinit
{
  namespace model
  {
    namespace doughnut
    {
      Async::Async(Doughnut& doughnut,
                   std::unique_ptr<Consensus> backend,
                   boost::filesystem::path journal_dir,
                   int max_size)
        : Consensus(doughnut)
        , _backend(std::move(backend))
        , _process_thread("async consensus", [&] { _process_loop();})
        , _next_index(1)
        , _journal_dir(journal_dir)
        , _restored_journal(false)
      {
        if (!_journal_dir.empty())
        {
          boost::filesystem::create_directories(_journal_dir);
        }
        if (max_size)
          _ops.max_size(max_size);
      }

      Async::~Async()
      {
        ELLE_TRACE("~Async");
        _process_thread.terminate_now();
        ELLE_TRACE("~~Async");
      }

      void
      Async::_restore_journal(overlay::Overlay& overlay)
      {
        ELLE_TRACE("Restoring journal from %s", _journal_dir);
        boost::filesystem::path p(_journal_dir);
        boost::filesystem::directory_iterator it(p);
        std::vector<boost::filesystem::path> files;
        while (it != boost::filesystem::directory_iterator())
        {
          files.push_back(it->path());
          ++it;
        }
        std::sort(files.begin(), files.end(),
          [](boost::filesystem::path const&a, boost::filesystem::path const& b) -> bool
          {
            return std::stoi(a.filename().string()) < std::stoi(b.filename().string());
          });
        for (auto const& p: files)
        {
          int id = std::stoi(p.filename().string());
          _next_index = std::max(id, _next_index);
          boost::filesystem::ifstream is(p);
          elle::serialization::binary::SerializerIn sin(is, false);
          sin.set_context<Model*>((Model*)&this->_doughnut);
          sin.set_context<Doughnut*>(&this->_doughnut);
          Op op(overlay);
          sin.set_context(ACBDontWaitForSignature{});
          sin.serialize("address", op.addr);
          sin.serialize("block", op.block);
          sin.serialize("mode", op.mode);
          sin.serialize("resolver", op.resolver);
          op.index = id;
          if (op.mode)
            _last[op.addr] = op.block.get();
          _ops.put(std::move(op));
        }
        _restored_journal = true;
      }

      void
      Async::_push_op(Op op)
      {
        op.index = ++_next_index;
        if (!_journal_dir.empty())
        {
          auto path = boost::filesystem::path(_journal_dir) / std::to_string(op.index);
          ELLE_DEBUG("creating %s", path);
          boost::filesystem::ofstream os(path);
          elle::serialization::binary::SerializerOut sout(os, false);
          sout.set_context(ACBDontWaitForSignature{});
          sout.set_context(OKBDontWaitForSignature{});
          sout.serialize("address", op.addr);
          sout.serialize("block", op.block);
          sout.serialize("mode", op.mode);
          sout.serialize("resolver", op.resolver);
        }
        _ops.put(std::move(op));
      }

      void
      Async::_store(overlay::Overlay& overlay,
                    std::unique_ptr<blocks::Block> block,
                    StoreMode mode,
                    std::unique_ptr<ConflictResolver> resolver)
      {
        if (!_restored_journal)
          _restore_journal(overlay);
        ELLE_TRACE("_store: %.7s", block->address());

        _last[block->address()] = block.get();
        _push_op(Op{overlay,
                    block->address(),
                    std::move(block),
                    mode,
                    std::move(resolver)
        });
      }

      void
      Async::_remove(overlay::Overlay& overlay,
              Address address)
      {
        if (!_restored_journal)
          _restore_journal(overlay);
        ELLE_TRACE("_remove: %.7s", address);
        _push_op({overlay, address, nullptr, {}});
      }

      // Fetch operation must be synchronious, else the consistency is not
      // preserved.
      std::unique_ptr<blocks::Block>
      Async::_fetch(overlay::Overlay& overlay,
                    Address address)
      {
        if (!_restored_journal)
          _restore_journal(overlay);
        ELLE_TRACE("_fetch: %.7s", address);
        if (_last.find(address) != _last.end())
        {
          ELLE_DUMP("_fetch: cache");
          auto cpy = _last[address]->clone();
          ELLE_DUMP("_fetch: cpy'd block data(%.7s): %s", cpy->address(), cpy->data());
          return cpy;
        }

        ELLE_DUMP("_fetch: network");
        return this->_backend->fetch(overlay, address);
      }

      void
      Async::_process_loop()
      {
        while (true)
        {
          try
          {
            Op op = _ops.get();

            overlay::Overlay& overlay = op.overlay;
            Address addr = op.addr;
            boost::optional<StoreMode> mode = op.mode;
            std::unique_ptr<ConflictResolver>& resolver = op.resolver;

            elle::SafeFinally delete_entry([&] {
                if (!_journal_dir.empty())
                {
                  auto path = boost::filesystem::path(_journal_dir) / std::to_string(op.index);
                  ELLE_DEBUG("deleting %s", path);
                  boost::filesystem::remove(path);
                }
            });

            if (!mode)
            {
              ELLE_TRACE("remove: %.7s", addr);
              this->_backend->remove(overlay, addr);
            }
            else // store
            {
              auto ptr = op.block.get();
              this->_backend->store(overlay,
                                    std::move(op.block),
                                    *mode,
                                    std::move(resolver));
              if (ptr == _last[addr])
              {
                _last.erase(addr);
                ELLE_DUMP("store: %.7s removed from cache", addr);
                for (auto const& i: _last)
                  ELLE_DUMP("store: _last[%.7s] = %p", i.first, i.second);
              }
            }
          }
          catch (reactor::Terminate const&)
          {
            ELLE_TRACE("Terminating thread");
            throw;
          }
          catch (std::exception const& e)
          {
            ELLE_WARN("Exception escaped Async loop: %s", e.what());
          }
        }
      }
    } // namespace doughnut
  } // namespace model
} // namespace infinit
