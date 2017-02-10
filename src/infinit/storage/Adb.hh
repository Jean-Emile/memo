#pragma once

#include <infinit/storage/Key.hh>
#include <infinit/storage/Storage.hh>

namespace infinit
{
  namespace storage
  {
    class Adb
      : public Storage
    {
    public:
      Adb(std::string const& root);
    protected:
      elle::Buffer
      _get(Key k) const override;
      int
      _set(Key k, elle::Buffer const& value, bool insert, bool update) override;
      int
      _erase(Key k) override;
      std::vector<Key>
      _list() override;
      ELLE_ATTRIBUTE(std::string, root);
    };

    struct AdbStorageConfig
      : public StorageConfig
    {
      AdbStorageConfig(std::string name,
                       boost::optional<int64_t> capacity,
                       boost::optional<std::string> description);
      AdbStorageConfig(elle::serialization::SerializerIn& input);

      void
      serialize(elle::serialization::Serializer& s) override;

      std::unique_ptr<infinit::storage::Storage>
      make() override;

      std::string root;
      std::shared_ptr<StorageConfig> storage;
    };
  }
}
