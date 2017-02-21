#include <infinit/model/Address.hh>

#include <boost/uuid/random_generator.hpp>

#include <elle/Buffer.hh>
#include <elle/format/hexadecimal.hh>

#include <cryptography/hash.hh>
#include <cryptography/random.hh>

namespace infinit
{
  namespace model
  {
    Address::Address()
      : _value()
      , _mutable_block(true)
    {
      memset(this->_value, 0, sizeof(Value));
    }

    Address::Address(Value const value, Flags flags, bool combine)
      : _value()
      , _mutable_block((flags & flags::block_kind) == flags::mutable_block)
    {
      ::memcpy(this->_value, value, sizeof(Value));
      if (combine)
        this->_value[flag_byte] = flags;
    }

    Address::Address(Value const value)
      : Address{value, value[flag_byte], false}
    {}

    Address::Address(elle::UUID const& id)
      : Address(infinit::cryptography::hash(
                  elle::ConstWeakBuffer(id.data, id.static_size()),
                  infinit::cryptography::Oneway::sha256).contents())
    {}

    int
    Address::cmp(Address const& rhs) const
    {
      return memcmp(this->_value, rhs._value, sizeof(Value));
    }

    bool
    Address::operator ==(Address const& rhs) const
    {
      return cmp(rhs) == 0;
    }

    bool
    Address::operator !=(Address const& rhs) const
    {
      return cmp(rhs) != 0;
    }

    bool
    Address::operator <(Address const& rhs) const
    {
      return cmp(rhs) < 0;
    }

    std::ostream&
    operator << (std::ostream& out, Address const& k)
    {
      if (out.flags() & std::ios::fixed)
      {
        out << "0x";
        out << elle::format::hexadecimal::encode(
          elle::ConstWeakBuffer(k._value, 4));
        out << elle::format::hexadecimal::encode(
          elle::ConstWeakBuffer(k._value + Address::flag_byte, 1));
      }
      else
        out << elle::ConstWeakBuffer(k._value);
      return out;
    }

    Address
    Address::from_string(std::string const& repr)
    {
      auto it = repr.begin();
      if (repr.length() >= 2 && repr[0] == '0' && repr[1] == 'x')
        it += 2;
      Value v;
      char c[3] = {0, 0, 0};
      for (int i = 0; i < 32; ++i)
      {
        if (it == repr.end())
          elle::err("invalid address: %s", repr);
        c[0] = *(it++);
        if (it == repr.end())
          elle::err("invalid address: %s", repr);
        c[1] = *(it++);
        v[i] = strtol(c, nullptr, 16);
      }
      if (it != repr.end())
        elle::err("invalid address: %s", repr);
      return {v};
    }

    Address
    Address::random()
    {
      auto buf =
        cryptography::random::generate<elle::Buffer>(sizeof(Value));
      ELLE_ASSERT_GTE(buf.size(), sizeof(Value));
      return Address(buf.contents());
    }

    Address
    Address::random(Flags flags)
    {
      return Address(Address::random().value(), flags, true);
    }

    Address const Address::null;

    std::size_t
    hash_value(Address const& address)
    {
      return std::hash<Address>()(address);
    }

    bool
    equal_unflagged(Address const& lhs, Address const& rhs)
    {
      static_assert(
        Address::flag_byte == 31,
        "this implementation is valid only if byte flag is the rightmost one");
      return memcmp(lhs.value(), rhs.value(), sizeof(Address::Value) - 1) == 0;
    }
  }
}

namespace std
{
  size_t
  std::hash<infinit::model::Address>::operator()(
    infinit::model::Address const& address) const
  {
    using boost::hash_combine;
    std::size_t res = 0;
    for (unsigned int i = 0;
         i < sizeof(infinit::model::Address::Value);
         i += sizeof(std::size_t))
      hash_combine(res,
                   *reinterpret_cast<std::size_t const*>(address.value() + i));
    return res;
  }
}

namespace elle
{
  namespace serialization
  {
    using infinit::model::Address;
    Serialize<Address>::Type
    Serialize<Address>::convert(Address& address)
    {
      return Type(address._value, sizeof(Address::Value));
    }

    Address
    Serialize<Address>::convert(Type buffer)
    {
      if (buffer.size() == 0)
        return Address();
      if (buffer.size() != sizeof(Address::Value))
        elle::err("invalid address: %x", buffer);
      Address::Value value;
      memcpy(value, buffer.contents(), sizeof(Address::Value));
      return Address(value);
    }
  }
}
