#include <infinit/storage/Crypt.hh>
#include <infinit/model/Address.hh>
#include <cryptography/SecretKey.hh>

#include <elle/factory.hh>

#include <boost/algorithm/string/classification.hpp>
#include <boost/algorithm/string/split.hpp>
ELLE_LOG_COMPONENT("infinit.fs.crypt");


namespace infinit
{
  namespace storage
  {

    static std::unique_ptr<infinit::storage::Storage> make(std::vector<std::string> const& args)
    {
      ELLE_TRACE_SCOPE("Processing crypt backend %s '%s', pass %s", args[0], args[1], args[2]);
      std::vector<std::string> bargs;
      size_t space = args[1].find(" ");
      size_t colon = args[1].find(":");
      const char* sep = (space == args[1].npos) ? ":" : " ";
      boost::algorithm::split(bargs, args[1], boost::algorithm::is_any_of(sep),
                              boost::algorithm::token_compress_on);
      std::unique_ptr<Storage> backend = elle::Factory<Storage>::instantiate(args[0], bargs);
      std::string const& password = args[2];
      bool salt = true;
      if (args.size() > 3)
      {
        std::string const& v = args[3];
        salt = (v=="1" || v == "yes" || v == "true");
      }
      // FIXME leaks, need ownership trasnfer API
      return elle::make_unique<Crypt>(*backend.release(),
                                      password, salt);
    }

    typedef  infinit::cryptography::SecretKey SecretKey;

    Crypt::Crypt(Storage& backend, std::string const& password,
            bool salt, // mix address and password to get a different key per block
            infinit::cryptography::cipher::Algorithm algorithm)
      : _backend(backend)
      , _password(password)
      , _salt(salt)
      , _algorithm(algorithm)
    {}
    elle::Buffer
    Crypt::_get(Key k) const
    {
      elle::Buffer e = _backend.get(k);
      SecretKey enc(_algorithm,
        _salt ? _password + elle::sprintf("%x", k) : _password);
      auto out = enc.decrypt<elle::Buffer>(infinit::cryptography::Output(e));
      return std::move(out);
    }
    void
    Crypt::_set(Key k, elle::Buffer const& value, bool insert, bool update)
    {
      SecretKey enc(_algorithm,
        _salt ? _password + elle::sprintf("%x", k) : _password);
      auto encrypted = enc.encrypt(value);
      _backend.set(k, encrypted.buffer(), insert, update);
    }
    void
    Crypt::_erase(Key k)
    {
      _backend.erase(k);
    }
  }
}

FACTORY_REGISTER(infinit::storage::Storage, "crypt", &infinit::storage::make);