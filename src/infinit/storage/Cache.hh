#pragma once

#include <set>

#include <infinit/storage/Storage.hh>

namespace infinit
{
  namespace storage
  {
    class Cache
      : public Storage
    {
    public:
      Cache(std::unique_ptr<Storage> backend,
            boost::optional<int> size = {},
            bool use_list = false, // optimize trusting keys from backend._list()
            bool use_status = true // optimize using backend._status()
            );
      std::string
      type() const override { return "cache"; }

    private:
      elle::Buffer
      _get(Key k) const override;
      int
      _set(Key k, elle::Buffer const& value, bool insert, bool update) override;
      int
      _erase(Key k) override;
      std::vector<Key>
      _list() override;
      void
      _init() const;

      ELLE_ATTRIBUTE_R(std::unique_ptr<Storage>, storage);
      ELLE_ATTRIBUTE_R(boost::optional<int>, size);
      ELLE_ATTRIBUTE_R(bool, use_list);
      ELLE_ATTRIBUTE_R(bool, use_status);
      using Blocks = std::unordered_map<Key, elle::Buffer>;
      ELLE_ATTRIBUTE(Blocks, blocks, mutable);
      using Keys = std::set<Key>;
      ELLE_ATTRIBUTE(boost::optional<Keys>, keys, mutable);
    };
  }
}
