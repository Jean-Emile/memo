#include <infinit/utility.hh>
#include <infinit/serialization.hh>

namespace infinit
{
  elle::Version serialization_tag::version(infinit::version());
  elle::unordered_map<elle::Version, elle::serialization::Serializer::Versions>
  serialization_tag::dependencies{{
    { elle::Version(0, 3, 0),
      {{ elle::type_info<elle::serialization_tag>(), elle::Version(0, 0, 0) }}},
    { elle::Version(0, 4, 0),
      {{ elle::type_info<elle::serialization_tag>(), elle::Version(0, 0, 0) }}},
    { elle::Version(0, 5, 0),
      {{ elle::type_info<elle::serialization_tag>(), elle::Version(0, 1, 0) }}},
    { elle::Version(0, 5, 1),
      {{ elle::type_info<elle::serialization_tag>(), elle::Version(0, 1, 0) }}},
    { elle::Version(0, 5, 2),
      {{ elle::type_info<elle::serialization_tag>(), elle::Version(0, 1, 0) }}},
    { elle::Version(0, 5, 3),
      {{ elle::type_info<elle::serialization_tag>(), elle::Version(0, 1, 0) }}},
    { elle::Version(0, 5, 4),
      {{ elle::type_info<elle::serialization_tag>(), elle::Version(0, 1, 0) }}},
    { elle::Version(0, 5, 5),
      {{ elle::type_info<elle::serialization_tag>(), elle::Version(0, 1, 0) }}},
    { elle::Version(0, 6, 0),
      {{ elle::type_info<elle::serialization_tag>(), elle::Version(0, 2, 0) }}},
    { elle::Version(0, 6, 1),
      {{ elle::type_info<elle::serialization_tag>(), elle::Version(0, 2, 0) }}},
    { elle::Version(0, 6, 2),
      {{ elle::type_info<elle::serialization_tag>(), elle::Version(0, 2, 0) }}},
    { elle::Version(0, 7, 0),
      {{ elle::type_info<elle::serialization_tag>(), elle::Version(0, 2, 0) }}},
  }};

  elle::Version
  elle_serialization_version(elle::Version const& infinit_version)
  {
    auto versions = elle::serialization::get_serialization_versions<
      infinit::serialization_tag>(infinit_version);
    return versions.at(elle::type_info<elle::serialization_tag>());
  }

}
