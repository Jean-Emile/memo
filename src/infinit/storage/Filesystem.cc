#include <infinit/storage/Filesystem.hh>

#include <iterator>

#include <boost/filesystem/fstream.hpp>
#include <boost/filesystem/operations.hpp>

#include <elle/factory.hh>
#include <elle/log.hh>

#include <infinit/storage/Collision.hh>
#include <infinit/storage/MissingKey.hh>

ELLE_LOG_COMPONENT("infinit.storage.Filesystem");

namespace infinit
{
  namespace storage
  {
    Filesystem::Filesystem(boost::filesystem::path root)
      : Storage()
      , _root(std::move(root))
    {}

    elle::Buffer
    Filesystem::_get(Key key) const
    {
      ELLE_TRACE("get %x", key);
      boost::filesystem::ifstream input(this->_path(key));
      if (!input.good())
        throw MissingKey(key);
      elle::Buffer res;
      elle::IOStream output(res.ostreambuf());
      std::copy(std::istreambuf_iterator<char>(input),
                std::istreambuf_iterator<char>(),
                std::ostreambuf_iterator<char>(output));
      return res;
    }

    void
    Filesystem::_set(Key key, elle::Buffer const& value, bool insert, bool update)
    {
      ELLE_TRACE("set %x", key);
      auto path = this->_path(key);
      bool exists = boost::filesystem::exists(path);
      if (!exists && !insert)
        throw MissingKey(key);
      if (exists && !update)
        throw Collision(key);
      boost::filesystem::ofstream output(path);
      if (!output.good())
        throw elle::Error(
          elle::sprintf("unable to open for writing: %s", path));
      output.write(reinterpret_cast<const char*>(value.contents()), value.size());
      if (insert && update)
        ELLE_DEBUG("%s: block %s", *this, exists ? "updated" : "inserted");
    }

    void
    Filesystem::_erase(Key key)
    {
      ELLE_TRACE("erase %x", key);
      auto path = this->_path(key);
      if (!exists(path))
        throw MissingKey(key);
      remove(path);
    }

    std::vector<Key>
    Filesystem::_list()
    {
      std::vector<Key> res;
      boost::filesystem::directory_iterator it(this->root());
      boost::filesystem::directory_iterator iend;
      while (it != iend)
      {
        std::string s = it->path().filename().string();
        if (s.substr(0, 2) != "0x" || s.length()!=66)
          continue;
        Key k = Key::from_string(s.substr(2));
        res.push_back(k);
        ++it;
      }
      return res;
    }

    boost::filesystem::path
    Filesystem::_path(Key const& key) const
    {
      return this->root() / elle::sprintf("%x", key);
    }

    static std::unique_ptr<Storage> make(std::vector<std::string> const& args)
    {
      return elle::make_unique<infinit::storage::Filesystem>(args[0]);
    }

    struct FilesystemStorageConfig
      : public StorageConfig
    {
    public:
      std::string path;

      FilesystemStorageConfig(elle::serialization::SerializerIn& input)
        : StorageConfig()
      {
        this->serialize(input);
      }

      void
      serialize(elle::serialization::Serializer& s)
      {
        s.serialize("path", this->path);
      }

      virtual
      std::unique_ptr<infinit::storage::Storage>
      make() override
      {
        return elle::make_unique<infinit::storage::Filesystem>(this->path);
      }
    };
    static const elle::serialization::Hierarchy<StorageConfig>::
    Register<FilesystemStorageConfig>
    _register_FilesystemStorageConfig("filesystem");
  }
}

FACTORY_REGISTER(infinit::storage::Storage, "filesystem", &infinit::storage::make);
