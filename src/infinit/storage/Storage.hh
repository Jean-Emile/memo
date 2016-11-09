#ifndef INFINIT_STORAGE_HH
# define INFINIT_STORAGE_HH

# include <iosfwd>
# include <cstdint>

# include <boost/signals2.hpp>

# include <elle/Buffer.hh>
# include <elle/attribute.hh>
# include <elle/optional.hh>
# include <elle/serialization/Serializer.hh>

# include <infinit/descriptor/TemplatedBaseDescriptor.hh>
# include <infinit/model/Address.hh>
# include <infinit/serialization.hh>
# include <infinit/storage/fwd.hh>

namespace infinit
{
  namespace storage
  {
    enum class BlockStatus
    {
      exists,
      missing,
      unknown
    };

    class Storage
    {
    public:
      Storage(boost::optional<int64_t> capacity = {});
      virtual
      ~Storage();
      /** Get the data associated to key \a k.
       *
       *  \param k Key of the looked-up data.
       *  \throw MissingKey if the key is absent.
       */
      elle::Buffer
      get(Key k) const;
      /** Set the data associated to key \a k.
       *
       *  \param k      Key of the set data.
       *  \param value  Value to associate to \a k.
       *  \param insert Whether to accept inserting a new key.
       *  \param update Whether to accept updating an exising key.
       *  \return The delta in used storage space in bytes.
       *  \throw Collision if the key is present and not \a update.
       *  \throw InsufficientSpace if there is not enough space left to store
       *                           the data.
       *  \throw MissingKey if the key is absent and not \a insert.
       */
      int
      set(Key k, elle::Buffer const& value,
          bool insert = true, bool update = false);
      /** Erase key \a k and associated data.
       *
       *  \param k      Key to remove.
       *  \return The delta in used storage space in bytes.
       *  \throw MissingKey if the key is absent.
       */
      int
      erase(Key k);
      /** List of all keys in the storage.
       *
       *  \return A list of all keys in the storage.
       */
      std::vector<Key>
      list();
      BlockStatus
      status(Key k);

      void
      register_notifier(std::function<void ()> f);
    protected:
      virtual
      elle::Buffer
      _get(Key k) const = 0;
      virtual
      int
      _set(Key k, elle::Buffer const& value, bool insert, bool update) = 0;
      virtual
      int
      _erase(Key k) = 0;
      virtual
      std::vector<Key>
      _list() = 0;
      /// Return the status of a given key.
      /// Implementations should check locally only if the information is
      /// available, or return BlockStatus::unknown.
      virtual
      BlockStatus
      _status(Key k);

      ELLE_ATTRIBUTE_R(boost::optional<int64_t>, capacity, protected);
      ELLE_ATTRIBUTE_R(int64_t, usage, protected);
      ELLE_ATTRIBUTE(int64_t, base_usage);
      ELLE_ATTRIBUTE(int64_t, step);
      ELLE_ATTRIBUTE((std::unordered_map<Key, int>), size_cache,
                     mutable, protected);
      ELLE_ATTRIBUTE(boost::signals2::signal<void ()>, on_storage_size_change);
    };

    std::unique_ptr<Storage>
    instantiate(std::string const& name,
                std::string const& args);

    struct StorageConfig
      : public descriptor::TemplatedBaseDescriptor<StorageConfig>
      , public elle::serialization::VirtuallySerializable<false>
    {
      StorageConfig() = default;
      StorageConfig(std::string name,
                    boost::optional<int64_t> capacity,
                    boost::optional<std::string> description);
      StorageConfig(elle::serialization::SerializerIn& input);
      virtual
      void
      serialize(elle::serialization::Serializer& s);
      typedef infinit::serialization_tag serialization_tag;
      static constexpr char const* virtually_serializable_key = "type";
      virtual
      std::unique_ptr<infinit::storage::Storage>
      make() = 0;

      boost::optional<int64_t> capacity;

      static
      std::string
      name_regex() ;
    };
  }
}


#endif
