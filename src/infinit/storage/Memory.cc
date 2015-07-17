#include <infinit/storage/Memory.hh>

#include <elle/factory.hh>
#include <elle/log.hh>

#include <infinit/storage/Collision.hh>
#include <infinit/storage/MissingKey.hh>

ELLE_LOG_COMPONENT("infinit.storage.Memory");

namespace infinit
{
  namespace storage
  {
    elle::Buffer
    Memory::_get(Key key) const
    {
      auto it = this->_blocks.find(key);
      if (it == this->_blocks.end())
        throw MissingKey(key);
      auto& buffer = it->second;
      return elle::Buffer(buffer.contents(), buffer.size());
    }

    void
    Memory::_set(Key key, elle::Buffer const& value, bool insert, bool update)
    {
      if (insert)
      {
        auto insertion =
          this->_blocks.insert(std::make_pair(key, elle::Buffer()));
        if (!insertion.second && !update)
          throw Collision(key);
        insertion.first->second = elle::Buffer(value.contents(), value.size());
        if (insert && update && insertion.second)
          ELLE_DEBUG("%s: block inserted", *this);
        else if (insert && update && insertion.second)
          ELLE_DEBUG("%s: block updated", *this);
      }
      else
      {
        auto search = this->_blocks.find(key);
        if (search == this->_blocks.end())
          throw MissingKey(key);
        else
          search->second = elle::Buffer(value.contents(), value.size());
      }
    }

    void
    Memory::_erase(Key key)
    {
      if (this->_blocks.erase(key) == 0)
        throw MissingKey(key);
    }

    std::vector<Key>
    Memory::_list()
    {
      std::vector<Key> res;
      for (auto const& b: this->_blocks)
        res.push_back(b.first);
      return res;
    }

    static std::unique_ptr<Storage> make(std::vector<std::string> const& args)
    {
      return elle::make_unique<infinit::storage::Memory>();
    }

    struct MemoryStorageConfig
      : public StorageConfig
    {
    public:
      MemoryStorageConfig(elle::serialization::SerializerIn& input)
        : StorageConfig()
      {
        this->serialize(input);
      }

      void
      serialize(elle::serialization::Serializer& s)
      {}

      virtual
      std::unique_ptr<infinit::storage::Storage>
      make() override
      {
        return elle::make_unique<infinit::storage::Memory>();
      }
    };
    static const elle::serialization::Hierarchy<StorageConfig>::
    Register<MemoryStorageConfig> _register_MemoryStorageConfig("memory");
  }
}

FACTORY_REGISTER(infinit::storage::Storage, "memory", &infinit::storage::make);
