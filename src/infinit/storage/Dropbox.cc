#include <infinit/storage/Dropbox.hh>

#include <elle/log.hh>

#include <infinit/storage/Collision.hh>
#include <infinit/storage/MissingKey.hh>

ELLE_LOG_COMPONENT("infinit.storage.Dropbox");

namespace infinit
{
  namespace storage
  {
    Dropbox::Dropbox(std::string token)
      : Dropbox(std::move(token), ".infinit")
    {}

    Dropbox::Dropbox(std::string token,
                     boost::filesystem::path root)
      : _dropbox(std::move(token))
      , _root(std::move(root))
    {}

    Dropbox::~Dropbox()
    {}

    static
    std::string
    _key_str(Key key)
    {
      return elle::sprintf("%x", key).substr(2);
    }

    boost::filesystem::path
    Dropbox::_path(Key key) const
    {
      return this->_root / _key_str(key);
    }

    elle::Buffer
    Dropbox::_get(Key key) const
    {
      ELLE_DEBUG("get %s", _key_str(key));
      try
      {
        ELLE_DEBUG("get path: %s", this->_path(key));
        return this->_dropbox.get(this->_path(key));
      }
      catch (dropbox::NoSuchFile const&)
      {
        throw MissingKey(key);
      }
    }

    int
    Dropbox::_set(Key key,
                  elle::Buffer const& value,
                  bool insert,
                  bool update)
    {
      ELLE_DEBUG("set %s", _key_str(key));
      if (insert)
      {
        auto insertion =
          this->_dropbox.put(this->_path(key), value, update);
        if (!insertion && !update)
          throw Collision(key);
      }
      else if (update)
      {
        ELLE_ABORT("not implemented (can dropbox handle it?)");
      }
      else
        throw elle::Error("neither inserting neither updating");

      // FIXME: impl.
      return 0;
    }

    int
    Dropbox::_erase(Key key)
    {
      ELLE_DEBUG("erase %s", _key_str(key));
      try
      {
        this->_dropbox.delete_(this->_path(key));
      }
      catch (dropbox::NoSuchFile const&)
      {
        throw MissingKey(key);
      }

      // FIXME: impl.
      return 0;
    }

    std::vector<Key>
    Dropbox::_list()
    {
      try
      {
        auto metadata = this->_dropbox.metadata("/" + this->_root.string());
        std::vector<Key> res;
        if (!metadata.is_dir)
        {
          throw elle::Error(
            elle::sprintf("%s is not a directory", this->_root.string()));
        }
        if  (!metadata.contents)
          return res;
        for (auto const& entry: metadata.contents.get())
        {
          std::string address =
            entry.path.substr(entry.path.find_last_of('/') + 1);
          res.push_back(model::Address::from_string(address));
        }
        return res;
      }
      catch (dropbox::NoSuchFile const& e)
      {
        return {};
      }
    }

    BlockStatus
    Dropbox::_status(Key k)
    {
      boost::filesystem::path p("/" + this->_root.string());
      p = p / _key_str(k);
      try
      {
        auto metadata = this->_dropbox.local_metadata(p);
        ELLE_DEBUG("status check on %x: %s", p, metadata? "exists" : "unknown");
        return metadata? BlockStatus::exists : BlockStatus::unknown;
      }
      catch (dropbox::NoSuchFile const &)
      {
        ELLE_DEBUG("status check on %s: %s", p, "missing");
        return BlockStatus::missing;
      }
    }

    DropboxStorageConfig::DropboxStorageConfig(
      std::string name,
      std::string token,
      boost::optional<std::string> root,
      boost::optional<int64_t> capacity,
      boost::optional<std::string> description)
      : StorageConfig(
          std::move(name), std::move(capacity), std::move(description))
      , token(std::move(token))
      , root(std::move(root))
    {}

    DropboxStorageConfig::DropboxStorageConfig(
      elle::serialization::SerializerIn& s)
      : StorageConfig(s)
      , token(s.deserialize<std::string>("token"))
      , root(s.deserialize<std::string>("root"))
    {}

    void
    DropboxStorageConfig::serialize(elle::serialization::Serializer& s)
    {
      StorageConfig::serialize(s);
      s.serialize("token", this->token);
      s.serialize("root", this->root);
    }

    std::unique_ptr<infinit::storage::Storage>
    DropboxStorageConfig::make()
    {
      if (this->root)
        return std::make_unique<infinit::storage::Dropbox>(
          this->token, this->root.get());
      else
        return std::make_unique<infinit::storage::Dropbox>(this->token);
    }

    static const elle::serialization::Hierarchy<StorageConfig>::
    Register<DropboxStorageConfig> _register_DropboxStorageConfig("dropbox");
  }
}
