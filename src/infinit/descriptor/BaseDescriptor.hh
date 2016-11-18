#ifndef INFINIT_DESCRIPTOR_BASE_DESCRIPTOR_HH
# define INFINIT_DESCRIPTOR_BASE_DESCRIPTOR_HH

# include <boost/filesystem/path_traits.hpp>
# include <boost/optional.hpp>

# include <elle/Printable.hh>

# include <das/model.hh>
# include <das/serializer.hh>

# include <infinit/symbols.hh>

namespace infinit
{
  namespace descriptor
  {
    /*-------.
    | Errors |
    `-------*/

    class DescriptorError
      : public elle::Error
    {
    public:
      DescriptorError(std::string const& message);
    };

    class DescriptorNameError
      : public DescriptorError
    {
    public:
      DescriptorNameError(std::string const& name);
    };

    class DescriptorDescriptionError
      : public DescriptorError
    {
    public:
      DescriptorDescriptionError(std::string const& description);
    };

    struct BaseDescriptor
      : public elle::Printable
    {
      /*-----.
      | Name |
      `-----*/

      class Name
        : public std::string
      {
      public:
        Name();
        Name(std::string name);
        Name(std::string const& qualifier, std::string const& name);

        std::string
        qualifier() const;
        std::string
        name() const;
        std::string
        unqualified(std::string const& qualifier) const;
      };

    /*-------------.
    | Construction |
    `-------------*/
    public:
      BaseDescriptor(std::string name,
                     boost::optional<std::string> description = {});
      BaseDescriptor(BaseDescriptor const& descriptor);
      Name name;
      boost::optional<std::string> description;
      using Model = das::Model<
        BaseDescriptor,
        elle::meta::List<symbols::Symbol_name, symbols::Symbol_description>>;

    /*--------------.
    | Serialization |
    `--------------*/
    public:
      BaseDescriptor(elle::serialization::SerializerIn& s);

      virtual
      void
      serialize(elle::serialization::Serializer& s);

    /*----------.
    | Printable |
    `----------*/
    public:
      virtual
      void
      print(std::ostream& out) const override;
    };
  }
}

namespace boost
{
  namespace filesystem
  {
    namespace path_traits
    {
      template <>
      struct is_pathable<infinit::descriptor::BaseDescriptor::Name>
      {
        static const bool value = true;
      };
    }
  }
}

#endif // INFINIT_DESCRIPTOR_BASE_DESCRIPTOR_HH