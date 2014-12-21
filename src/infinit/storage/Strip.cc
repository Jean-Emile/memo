#include <infinit/storage/Strip.hh>
#include <infinit/model/Address.hh>

#include <boost/algorithm/string.hpp>

#include <elle/factory.hh>
#include <reactor/Scope.hh>

namespace infinit
{
  namespace storage
  {
    Strip::Strip(std::vector<Storage*> backend)
      : _backend(backend)
    {
    }
    elle::Buffer
    Strip::_get(Key k) const
    {
      return _backend[_disk_of(k)]->get(k);
    }
    void
    Strip::_set(Key k, elle::Buffer const& value, bool insert, bool update)
    {
      _backend[_disk_of(k)]->set(k, value, insert, update);
    }
    void
    Strip::_erase(Key k)
    {
      _backend[_disk_of(k)]->erase(k);
    }
    int
    Strip::_disk_of(Key k) const
    {
      auto value = k.value();
      int res = 0;
      for (unsigned i=0; i<sizeof(Key::Value); ++i)
        res += value[i];
      return res % _backend.size();
    }
    static std::unique_ptr<Storage> make(std::vector<std::string> const& args)
    {
      std::vector<Storage*> backends;
      for (int i=0; i<args.size(); i+=2)
      {
        std::string name = args[i];
        std::vector<std::string> bargs;
        size_t space = args[i+1].find(" ");
        size_t colon = args[i+1].find(":");
        const char* sep = (space == args[i+1].npos) ? ":" : " ";
        boost::algorithm::split(bargs, args[i+1], boost::algorithm::is_any_of(sep),
                                boost::algorithm::token_compress_on);
        std::unique_ptr<Storage> backend = elle::Factory<Storage>::instantiate(name, bargs);
        backends.push_back(backend.release());
      }
      return elle::make_unique<Strip>(backends);
    }
  }
}

FACTORY_REGISTER(infinit::storage::Storage, "strip", &infinit::storage::make);