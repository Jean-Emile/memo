#ifndef INFINIT_STORAGE_DROPBOX_HH
# define INFINIT_STORAGE_DROPBOX_HH

# include <dropbox/Dropbox.hh>

# include <infinit/storage/Key.hh>
# include <infinit/storage/Storage.hh>

namespace infinit
{
  namespace storage
  {
    class Dropbox
      : public Storage
    {
    public:
      Dropbox(std::string token);
      Dropbox(std::string token,
              boost::filesystem::path root);
      ~Dropbox();

    protected:
      virtual
      elle::Buffer
      _get(Key k) const override;
      virtual
      void
      _set(Key k, elle::Buffer const& value, bool insert, bool update) override;
      virtual
      void
      _erase(Key k);
      virtual
      std::vector<Key>
      _list() override;
      ELLE_ATTRIBUTE(dropbox::Dropbox, dropbox);
      ELLE_ATTRIBUTE_R(boost::filesystem::path, root);

    private:
      boost::filesystem::path
      _path(Key key) const;
    };

    struct DropboxStorageConfig
      : public StorageConfig
    {
      DropboxStorageConfig(std::string token,
                           boost::optional<std::string> root);
      DropboxStorageConfig(elle::serialization::SerializerIn& input);
      void
      serialize(elle::serialization::Serializer& s);
      virtual
      std::unique_ptr<infinit::storage::Storage>
      make() override;

      std::string token;
      boost::optional<std::string> root;
    };

  }
}

#endif
