#include <infinit/storage/Crypt.hh>
#include <infinit/model/Address.hh>
#include <elle/cryptography/SecretKey.hh>

#include <elle/factory.hh>

namespace infinit
{
  namespace storage
  {

    static
    std::unique_ptr<infinit::storage::Storage>
    make(std::vector<std::string> const& args)
    {
      std::unique_ptr<Storage> backend = instantiate(args[0], args[1]);
      std::string const& password = args[2];
      bool salt = true;
      if (args.size() > 3)
      {
        std::string const& v = args[3];
        salt = (v == "1" || v == "yes" || v == "true");
      }
      return std::make_unique<Crypt>(std::move(backend), password, salt);
    }

    typedef  elle::cryptography::SecretKey SecretKey;

    Crypt::Crypt(std::unique_ptr<Storage> backend,
                 std::string const& password,
                 bool salt)
      : _backend(std::move(backend))
      , _password(password)
      , _salt(salt)
    {}

    elle::Buffer
    Crypt::_get(Key k) const
    {
      elle::Buffer e = this->_backend->get(k);
      SecretKey enc(_salt ? _password + elle::sprintf("%x", k) : _password);
      return enc.decipher(e);
    }

    int
    Crypt::_set(Key k, elle::Buffer const& value, bool insert, bool update)
    {
      SecretKey enc(
        _salt ? _password + elle::sprintf("%x", k) : this->_password);
      this->_backend->set(k, enc.encipher(value), insert, update);

      return 0;
    }

    int
    Crypt::_erase(Key k)
    {
      this->_backend->erase(k);
      return 0;
    }

    std::vector<Key>
    Crypt::_list()
    {
      return this->_backend->list();
    }

    CryptStorageConfig::CryptStorageConfig(
      std::string name,
      boost::optional<int64_t> capacity,
      boost::optional<std::string> description)
      : StorageConfig(name, std::move(capacity), std::move(description))
    {}

    CryptStorageConfig::CryptStorageConfig(elle::serialization::SerializerIn& s)
      : StorageConfig(s)
      , password(s.deserialize<std::string>("password"))
      , salt(s.deserialize<bool>("salt"))
      , storage(s.deserialize<std::shared_ptr<StorageConfig>>("backend"))
    {}

    void
    CryptStorageConfig::serialize(elle::serialization::Serializer& s)
    {
      StorageConfig::serialize(s);
      s.serialize("password", this->password);
      s.serialize("salt", this->salt);
      s.serialize("backend", this->storage);
    }

    std::unique_ptr<infinit::storage::Storage>
    CryptStorageConfig::make()
    {
      return std::make_unique<infinit::storage::Crypt>(
        storage->make(), password, salt);
    }


    static const elle::serialization::Hierarchy<StorageConfig>::
    Register<CryptStorageConfig>
    _register_CryptStorageConfig("crypt");
  }
}

FACTORY_REGISTER(infinit::storage::Storage, "crypt", &infinit::storage::make);
