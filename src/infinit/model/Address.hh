#ifndef INFINIT_MODEL_ADDRESS_HH
# define INFINIT_MODEL_ADDRESS_HH

# include <cstdint>
# include <cstring>
# include <functional>
# include <utility>

# include <elle/UUID.hh>
# include <elle/attribute.hh>
# include <elle/serialization/Serializer.hh>

namespace infinit
{
  namespace model
  {
    using namespace std::rel_ops;

    namespace flags
    {
      static const uint8_t block_kind = 1;
      static const uint8_t mutable_block = 0;
      static const uint8_t immutable_block = 1;
    }

    class Address
    {
    public:
      using Value = uint8_t[32];
      using Flags = uint8_t;
      static constexpr int flag_byte = 31;
      Address();
      Address(Value const value);
      Address(Value const value, Flags flags, bool combine);
      Address(elle::UUID const& id);
      /// Ternary comparison, as with memcmp.
      int
      cmp(Address const& rhs) const;
      bool
      operator ==(Address const& rhs) const;
      bool
      operator !=(Address const& rhs) const;
      bool
      operator <(Address const& rhs) const;
      friend
      std::ostream&
      operator << (std::ostream& out, Address const& k);
      static
      Address
      from_string(std::string const& repr);
      static
      Address
      random();
      static
      Address
      random(Flags flags);
      static Address const null;
    private:
      friend
      struct elle::serialization::Serialize<infinit::model::Address>;
      ELLE_ATTRIBUTE_R(Value, value);
      ELLE_ATTRIBUTE_R(bool, mutable_block);
    };

    std::ostream&
    operator << (std::ostream& out, Address const& k);

    std::size_t
    hash_value(Address const& address);

    /// Compare excluding flag byte, for backward compatibility
    bool
    equal_unflagged(Address const& lhs, Address const& rhs);
  }
}

namespace std
{
  template <>
  struct hash<infinit::model::Address>
  {
    size_t
    operator()(infinit::model::Address const& employee) const;
  };
}

namespace elle
{
  namespace serialization
  {
    template <>
    struct Serialize<infinit::model::Address>
    {
      typedef elle::Buffer Type;
      static
      Type
      convert(infinit::model::Address& address);
      static
      infinit::model::Address
      convert(Type repr);
    };
  }
}

#endif
