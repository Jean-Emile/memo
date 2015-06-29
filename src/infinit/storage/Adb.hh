#ifndef INFINIT_STORAGE_ADB_HH
# define INFINIT_STORAGE_ADB_HH

# include <infinit/storage/Key.hh>
# include <infinit/storage/Storage.hh>

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
      virtual
      elle::Buffer
      _get(Key k) const override;
      virtual
      void
      _set(Key k, elle::Buffer const& value, bool insert, bool update) override;
      virtual
      void
      _erase(Key k) override;
      virtual
      std::vector<Key>
      _list() override;
      ELLE_ATTRIBUTE(std::string, root);
    };
  }
}

#endif