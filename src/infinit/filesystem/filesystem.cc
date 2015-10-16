#include <infinit/filesystem/filesystem.hh>
#include <infinit/filesystem/AnyBlock.hh>
#include <infinit/filesystem/Directory.hh>
#include <infinit/filesystem/umbrella.hh>

#include <infinit/model/MissingBlock.hh>

#include <elle/cast.hh>
#include <elle/log.hh>
#include <elle/os/environ.hh>
#include <elle/unordered_map.hh>

#include <elle/serialization/Serializer.hh>
#include <elle/serialization/json/SerializerIn.hh>
#include <elle/serialization/json/SerializerOut.hh>

#include <reactor/filesystem.hh>
#include <reactor/scheduler.hh>
#include <reactor/exception.hh>

#include <cryptography/hash.hh>

#include <infinit/model/Address.hh>
#include <infinit/model/blocks/MutableBlock.hh>
#include <infinit/model/blocks/ImmutableBlock.hh>
#include <infinit/model/blocks/ACLBlock.hh>
#include <infinit/model/doughnut/Doughnut.hh>
#include <infinit/model/doughnut/User.hh>
#include <infinit/model/doughnut/ValidationFailed.hh>
#include <infinit/model/doughnut/Conflict.hh>
#include <infinit/model/doughnut/NB.hh>
#include <infinit/model/doughnut/ACB.hh>
#include <infinit/serialization.hh>


#ifdef INFINIT_LINUX
  #include <attr/xattr.h>
#endif

ELLE_LOG_COMPONENT("infinit.fs");

namespace rfs = reactor::filesystem;

namespace infinit
{
  namespace filesystem
  {
    FileSystem::FileSystem(std::string const& volume_name,
                           std::shared_ptr<model::Model> model)
      : _block_store(std::move(model))
      , _single_mount(false)
      , _volume_name(volume_name)
    {
      reactor::scheduler().signal_handle
        (SIGUSR1, [this] { this->print_cache_stats();});
    }

    void
    FileSystem::unchecked_remove(model::Address address)
    {
      try
      {
        _block_store->remove(address);
      }
      catch (model::MissingBlock const&)
      {
        ELLE_DEBUG("%s: block was not published", *this);
      }
      catch (elle::Exception const& e)
      {
        ELLE_ERR("%s: unexpected exception: %s", *this, e.what());
        throw;
      }
      catch (...)
      {
        ELLE_ERR("%s: unknown exception", *this);
        throw;
      }
    }

    void
    FileSystem::store_or_die(std::unique_ptr<model::blocks::Block> block,
                             model::StoreMode mode)
    {
      ELLE_TRACE_SCOPE("%s: store or die: %s", *this, *block);

      try
      {
        this->_block_store->store(std::move(block), mode);
      }
      catch (infinit::model::doughnut::ValidationFailed const& e)
      {
        ELLE_TRACE("permission exception: %s", e.what());
        throw rfs::Error(EACCES, elle::sprintf("%s", e.what()));
      }
      catch(elle::Error const& e)
      {
        ELLE_WARN("unexpected exception storing %x: %s",
                  block->address(), e);
        throw rfs::Error(EIO, e.what());
      }
    }

    std::unique_ptr<model::blocks::Block>
    FileSystem::fetch_or_die(model::Address address)
    {
      try
      {
        return _block_store->fetch(address);
      }
      catch(reactor::Terminate const& e)
      {
        throw;
      }
      catch (infinit::model::doughnut::ValidationFailed const& e)
      {
        ELLE_TRACE("perm exception %s", e);
        throw rfs::Error(EACCES, elle::sprintf("%s", e));
      }
      catch (model::MissingBlock const& mb)
      {
        ELLE_WARN("unexpected storage result fetching: %s", mb);
        throw rfs::Error(EIO, elle::sprintf("%s", mb));
      }
      catch (elle::serialization::Error const& se)
      {
        ELLE_WARN("serialization error fetching %x: %s", address, se);
        throw rfs::Error(EIO, elle::sprintf("%s", se));
      }
      catch(elle::Exception const& e)
      {
        ELLE_WARN("unexpected exception fetching %x: %s", address, e);
        throw rfs::Error(EIO, elle::sprintf("%s", e));
      }
      catch(std::exception const& e)
      {
        ELLE_WARN("unexpected exception on fetching %x: %s", address, e);
        throw rfs::Error(EIO, e.what());
      }
    }

    std::unique_ptr<model::blocks::MutableBlock>
    FileSystem::unchecked_fetch(model::Address address)
    {
      try
      {
        return elle::cast<model::blocks::MutableBlock>::runtime
          (_block_store->fetch(address));
      }
      catch (model::MissingBlock const& mb)
      {
        ELLE_WARN("Unexpected storage result: %s", mb);
      }
      return {};
    }

    void
    FileSystem::print_cache_stats()
    {
      auto root = std::dynamic_pointer_cast<Directory>(filesystem()->path("/"));
      CacheStats stats;
      memset(&stats, 0, sizeof(CacheStats));
      root->cache_stats(stats);
      std::cerr << "Statistics:\n"
      << stats.directories << " dirs\n"
      << stats.files << " files\n"
      << stats.blocks <<" blocks\n"
      << stats.size << " bytes"
      << std::endl;
    }

    std::shared_ptr<rfs::Path>
    FileSystem::path(std::string const& path)
    {
      ELLE_TRACE_SCOPE("%s: fetch root", *this);
      // In the infinit filesystem, we never query a path other than the root.
      ELLE_ASSERT_EQ(path, "/");
      auto root = this->_root_block();
      ELLE_ASSERT(!!root);
      auto acl_root =  elle::cast<ACLBlock>::runtime(std::move(root));
      ELLE_ASSERT(!!acl_root);
      auto res =
        std::make_shared<Directory>(nullptr, *this, "", acl_root->address());
      res->_block = std::move(acl_root);
      return res;
    }

    std::unique_ptr<MutableBlock>
    FileSystem::_root_block()
    {
      auto dn =
        std::dynamic_pointer_cast<model::doughnut::Doughnut>(_block_store);
      Address addr =
        model::doughnut::NB::address(dn->owner(), _volume_name + ".root");
      while (true)
      {
        try
        {
          ELLE_DEBUG_SCOPE("fetch root boostrap block at %x", addr);
          auto block = _block_store->fetch(addr);
          addr = Address::from_string(block->data().string().substr(2));
          break;
        }
        catch (model::MissingBlock const& e)
        {
          if (dn->owner() == dn->keys().K())
          {
            ELLE_TRACE("create missing root bootsrap block");
            std::unique_ptr<MutableBlock> mb = dn->make_block<ACLBlock>();
            auto saddr = elle::sprintf("%x", mb->address());
            elle::Buffer baddr = elle::Buffer(saddr.data(), saddr.size());
            auto cpy = mb->clone();
            store_or_die(std::move(cpy), model::STORE_INSERT);
            auto nb = elle::make_unique<model::doughnut::NB>(
                dn.get(), dn->owner(), this->_volume_name + ".root", baddr);
            store_or_die(std::move(nb), model::STORE_INSERT);
            return mb;
          }
          reactor::sleep(1_sec);
        }
      }
      return elle::cast<MutableBlock>::runtime(fetch_or_die(addr));
    }
  }
}
