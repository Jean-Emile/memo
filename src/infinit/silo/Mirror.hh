#pragma once

#include <infinit/silo/Storage.hh>

namespace infinit
{
  namespace silo
  {
    class Mirror: public Storage
    {
    public:
      Mirror(std::vector<std::unique_ptr<Storage>> backend, bool balance_reads,
             bool parallel = true);
      std::string
      type() const override { return "mirror"; }

    protected:
      elle::Buffer
      _get(Key k) const override;
      int
      _set(Key k, elle::Buffer const& value, bool insert, bool update) override;
      int
      _erase(Key k) override;
      std::vector<Key>
      _list() override;

      ELLE_ATTRIBUTE(bool, balance_reads);
      ELLE_ATTRIBUTE(std::vector<std::unique_ptr<Storage>>, backend);
      ELLE_ATTRIBUTE(unsigned int, read_counter);
      ELLE_ATTRIBUTE(bool, parallel);
    };
  }
}
