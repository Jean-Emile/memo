#ifndef INFINIT_MODEL_DOUGHNUT_USER_HH
# define INFINIT_MODEL_DOUGHNUT_USER_HH

# include <elle/cryptography/rsa/PublicKey.hh>

# include <infinit/model/User.hh>

namespace infinit
{
  namespace model
  {
    namespace doughnut
    {
      class User
        : public model::User
      {
      public:
        User(elle::cryptography::rsa::PublicKey key, std::string const& name);
        std::string name() override;
        ELLE_ATTRIBUTE_R(elle::cryptography::rsa::PublicKey, key);
        ELLE_ATTRIBUTE_R(std::string, name);
      };
    }
  }
}

#endif
