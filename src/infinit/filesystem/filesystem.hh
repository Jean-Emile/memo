#ifndef INFINIT_FILESYSTEM_FILESYSTEM_HH
# define INFINIT_FILESYSTEM_FILESYSTEM_HH

# include <chrono>

# include <boost/multi_index/hashed_index.hpp>
# include <boost/multi_index/identity.hpp>
# include <boost/multi_index/mem_fun.hpp>
# include <boost/multi_index/ordered_index.hpp>
# include <boost/multi_index/sequenced_index.hpp>
# include <boost/multi_index_container.hpp>

# include <cryptography/rsa/KeyPair.hh>

# include <reactor/filesystem.hh>
# include <reactor/thread.hh>

# include <infinit/model/Model.hh>
# include <infinit/filesystem/FileData.hh>

namespace infinit
{
  namespace filesystem
  {
    namespace bmi = boost::multi_index;
    typedef model::blocks::Block Block;
    typedef model::blocks::ACLBlock ACLBlock;
    class FileSystem;
    class FileBuffer;
    enum class EntryType
    {
      file,
      directory,
      symlink,
      pending
    };
    enum class OperationType
    {
      insert,
      update,
      remove,
      insert_exclusive,
    };
    struct Operation
    {
      OperationType type;
      std::string target;
      EntryType entry_type;
      Address address;
    };

    class DirectoryData
    {
    public:
      using clock = std::chrono::high_resolution_clock;
      static std::unique_ptr<model::blocks::ACLBlock> null_block;
      DirectoryData(boost::filesystem::path path,
                    Block& block, std::pair<bool, bool> perms);
      DirectoryData(boost::filesystem::path path,
                    model::Address address);
      DirectoryData(elle::serialization::Serializer& s, elle::Version const& v);
      void
      update(model::blocks::Block& block, std::pair<bool, bool> perms);
      void
      write(FileSystem& fs,
            Operation op,
            std::unique_ptr<model::blocks::ACLBlock>&block = null_block,
            bool set_mtime = false,
            bool first_write = false);
      void
      _prefetch(FileSystem& fs, std::shared_ptr<DirectoryData> self);
      void
      serialize(elle::serialization::Serializer&, elle::Version const& v);
      typedef infinit::serialization_tag serialization_tag;
      ELLE_ATTRIBUTE_R(model::Address, address);
      ELLE_ATTRIBUTE_R(int, block_version);
      typedef elle::unordered_map<std::string, std::pair<EntryType, model::Address>> Files;
      ELLE_ATTRIBUTE_R(FileHeader, header);
      ELLE_ATTRIBUTE_R(Files, files);
      ELLE_ATTRIBUTE_R(bool, inherit_auth);
      ELLE_ATTRIBUTE_R(bool, prefetching);
      ELLE_ATTRIBUTE_R(clock::time_point, last_prefetch);
      ELLE_ATTRIBUTE_R(clock::time_point, last_used);
      ELLE_ATTRIBUTE_R(boost::filesystem::path, path);
      friend class Unknown;
      friend class Directory;
      friend class File;
      friend class Node;
      friend class FileSystem;
      friend class Symlink;
      friend std::unique_ptr<Block>
      resolve_directory_conflict(Block& b,
                                 Block& current,
                                 model::StoreMode store_mode,
                                 model::Model& model,
                                 Operation op,
                                 Address address,
                                 bool deserialized);
    };

    enum class WriteTarget
    {
      none = 0,
      perms = 1,
      links = 2,
      data = 4,
      times = 8,
      xattrs = 16,
      symlink = 32,
      all = 255,
      block = 32768,
    };
    inline
    bool
    operator &(WriteTarget const& l, WriteTarget const& r)
    {
      typedef std::underlying_type<WriteTarget>::type ut;
      return static_cast<ut>(l) & static_cast<ut>(r);
    }
    inline
    WriteTarget
    operator |(WriteTarget const& l, WriteTarget const& r)
    {
      typedef std::underlying_type<WriteTarget>::type ut;
      return static_cast<WriteTarget>(
        static_cast<ut>(l) | static_cast<ut>(r));
    }
    class FileData
    {
    public:
      using clock = std::chrono::high_resolution_clock;
      FileData(boost::filesystem::path path,
               Block& block, std::pair<bool, bool> perms);
      FileData(boost::filesystem::path path,
               model::Address address, int mode);
      void
      update(model::blocks::Block& block, std::pair<bool, bool> perms);
      void
      write(FileSystem& fs,
            WriteTarget target = WriteTarget::all,
            std::unique_ptr<ACLBlock>&block = DirectoryData::null_block,
            bool first_write = false);
      void
      merge(const FileData& previous, WriteTarget target);
      ELLE_ATTRIBUTE_R(model::Address, address);
      ELLE_ATTRIBUTE_R(int, block_version);
      ELLE_ATTRIBUTE_R(clock::time_point, last_used);
      ELLE_ATTRIBUTE_R(FileHeader, header);
      typedef std::pair<Address, std::string> FatEntry; // (address, key)
      ELLE_ATTRIBUTE_R(std::vector<FatEntry>, fat);
      ELLE_ATTRIBUTE_R(elle::Buffer, data);
      ELLE_ATTRIBUTE_R(boost::filesystem::path, path);
      typedef infinit::serialization_tag serialization_tag;
      friend class FileSystem;
      friend class File;
      friend class FileHandle;
      friend class FileBuffer;
      friend class FileConflictResolver;
    };
    class Node;
    void unchecked_remove(model::Model& model,
                          model::Address address);
    std::unique_ptr<model::blocks::Block>
    fetch_or_die(model::Model& model,
                 model::Address address,
                 boost::optional<int> local_version = {},
                 boost::filesystem::path const& path = {});
    std::pair<bool, bool>
    get_permissions(model::Model& model,
                    model::blocks::Block const& block);
    DAS_SYMBOL(allow_root_creation);
    DAS_SYMBOL(model);
    DAS_SYMBOL(mountpoint);
    DAS_SYMBOL(owner);
    DAS_SYMBOL(root_block_cache_dir);
    DAS_SYMBOL(volume_name);
    DAS_SYMBOL(map_other_permissions);
    /** Filesystem using a Block Storage as backend.
    * Directory: nodes are serialized, and contains name, stat() and block
    *            address of the directory content
    * File    : In direct mode, one block with all the data
    *           In index mode, one block containing headers
    *           and the list of addresses for the content.
    */
    class FileSystem
      : public reactor::filesystem::Operations
    {
    /*------.
    | Types |
    `------*/
    public:
      using clock = std::chrono::high_resolution_clock;

    /*-------------.
    | Construction |
    `-------------*/
    public:
      template <typename ... Args>
      FileSystem(Args&& ... args);
      FileSystem(
        std::string volume_name,
        std::shared_ptr<infinit::model::Model> model,
        boost::optional<infinit::cryptography::rsa::PublicKey> owner = {},
        boost::optional<boost::filesystem::path> root_block_cache_dir = {},
        boost::optional<boost::filesystem::path> mountpoint = {},
        bool allow_root_creation = false,
        bool map_other_permissions = true);
      ~FileSystem();
    private:
      struct Init;
      FileSystem(Init);

    public:
      static
      clock::time_point
      now();
      void
      print_cache_stats();
      std::shared_ptr<reactor::filesystem::Path>
      path(std::string const& path) override;

      void unchecked_remove(model::Address address);
      std::unique_ptr<model::blocks::MutableBlock>
      unchecked_fetch(model::Address address);

      std::unique_ptr<model::blocks::Block>
      fetch_or_die(model::Address address,
                   boost::optional<int> local_version = {},
                   boost::filesystem::path const& path = {});

      void
      store_or_die(std::unique_ptr<model::blocks::Block> block,
                   model::StoreMode mode,
                   std::unique_ptr<model::ConflictResolver> resolver = {});
      void
      store_or_die(model::blocks::Block& block,
                   model::StoreMode mode,
                   std::unique_ptr<model::ConflictResolver> resolver = {});
      // Check permissions and throws on access failure
      void
      ensure_permissions(model::blocks::Block const& block, bool r, bool w);

      boost::signals2::signal<void()> on_root_block_create;
      std::shared_ptr<DirectoryData>
      get(boost::filesystem::path path, model::Address address);
      void filesystem(reactor::filesystem::FileSystem* fs) override;
      reactor::filesystem::FileSystem* filesystem();
    private:
      Address
      root_address();
    public:
      infinit::cryptography::rsa::PublicKey const&
      owner() const;
      ELLE_ATTRIBUTE_R(std::shared_ptr<infinit::model::Model>, block_store);
      ELLE_ATTRIBUTE_RW(bool, single_mount);
      ELLE_ATTRIBUTE(boost::optional<infinit::cryptography::rsa::PublicKey>, owner);
      ELLE_ATTRIBUTE_R(std::string, volume_name);
      ELLE_ATTRIBUTE_R(std::string, network_name);
      ELLE_ATTRIBUTE_R(bool, read_only);
      ELLE_ATTRIBUTE_R(boost::optional<boost::filesystem::path>,
                       root_block_cache_dir);
      ELLE_ATTRIBUTE_R(boost::optional<boost::filesystem::path>, mountpoint);
      ELLE_ATTRIBUTE_R(model::Address, root_address);
      ELLE_ATTRIBUTE_R(bool, allow_root_creation);
      ELLE_ATTRIBUTE_R(bool, map_other_permissions);

      typedef bmi::multi_index_container<
        std::shared_ptr<DirectoryData>,
        bmi::indexed_by<
          bmi::hashed_unique<
            bmi::const_mem_fun<
              DirectoryData,
              Address const&, &DirectoryData::address>>,
          bmi::ordered_non_unique<
            bmi::const_mem_fun<
              DirectoryData,
              clock::time_point const&, &DirectoryData::last_used>>
              > > DirectoryCache;
      ELLE_ATTRIBUTE_R(DirectoryCache, directory_cache);
      typedef bmi::multi_index_container<
        std::shared_ptr<FileData>,
        bmi::indexed_by<
          bmi::hashed_unique<
            bmi::const_mem_fun<
              FileData,
              Address const&, &FileData::address>>,
          bmi::ordered_non_unique<
            bmi::const_mem_fun<
              FileData,
              clock::time_point const&, &FileData::last_used>>
              > > FileCache;
      ELLE_ATTRIBUTE_R(FileCache, file_cache);
      ELLE_ATTRIBUTE_RX(std::vector<reactor::Thread::unique_ptr>, running);
      ELLE_ATTRIBUTE_RX(int, prefetching);
      typedef
      std::unordered_map<Address, std::weak_ptr<FileBuffer>>
      FileBuffers;
      ELLE_ATTRIBUTE_RX(FileBuffers, file_buffers);
      static const int max_cache_size = 10000;
      friend class FileData;
      friend class DirectoryData;
    };
  }
}

namespace std
{
  std::ostream&
  operator <<(std::ostream& out,
              infinit::filesystem::EntryType entry);

  std::ostream&
  operator <<(std::ostream& out,
              infinit::filesystem::OperationType operation);
}

# include <infinit/filesystem/Filesystem.hxx>

#endif
