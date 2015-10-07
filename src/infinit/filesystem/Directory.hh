#ifndef INFINIT_FILESYSTEM_DIRECTORY_HH
# define INFINIT_FILESYSTEM_DIRECTORY_HH

#include <reactor/filesystem.hh>
#include <infinit/filesystem/Node.hh>
#include <infinit/filesystem/umbrella.hh>
#include <infinit/filesystem/FileData.hh>
#include <elle/unordered_map.hh>

namespace infinit
{
  namespace filesystem
  {
    namespace rfs = reactor::filesystem;
    typedef std::shared_ptr<Directory> DirectoryPtr;
    typedef infinit::model::blocks::ACLBlock ACLBlock;

    struct CacheStats
    {
      int directories;
      int files;
      int blocks;
      long size;
    };

    enum class OperationType
    {
      insert,
      update,
      remove
    };

    struct Operation
    {
      OperationType type;
      std::string target;
    };

    static const int DIRECTORY_MASK = 0040000;
    static const int SYMLINK_MASK = 0120000; 
    static const boost::posix_time::time_duration directory_cache_time
      = boost::posix_time::seconds(2);

    class Directory
      : public rfs::Path
      , public Node
    {
      public:
        Directory(DirectoryPtr parent, FileSystem& owner, std::string const& name,
            Address address);
        void stat(struct stat*) override;
        void list_directory(rfs::OnDirectoryEntry cb) override;
        std::unique_ptr<rfs::Handle> open(int flags, mode_t mode) override THROW_ISDIR;
        std::unique_ptr<rfs::Handle> create(int flags, mode_t mode) override THROW_ISDIR;
        void unlink() override THROW_ISDIR;
        void mkdir(mode_t mode) override THROW_EXIST;
        void rmdir() override;
        void rename(boost::filesystem::path const& where) override;
        boost::filesystem::path readlink() override  THROW_ISDIR;
        void symlink(boost::filesystem::path const& where) override THROW_EXIST;
        void link(boost::filesystem::path const& where) override THROW_EXIST;
        void chmod(mode_t mode) override;
        void chown(int uid, int gid) override;
        void statfs(struct statvfs *) override;
        void utimens(const struct timespec tv[2]) override;
        void truncate(off_t new_size) override THROW_ISDIR;
        std::shared_ptr<rfs::Path> child(std::string const& name) override;
        std::string getxattr(std::string const& key) override;
        std::vector<std::string> listxattr() override;
        void setxattr(std::string const& name, std::string const& value, int flags) override;
        void removexattr(std::string const& name) override;
        void cache_stats(CacheStats& append);
        void serialize(elle::serialization::Serializer&);
        bool allow_cache() override { return true;}
        virtual
          void
          print(std::ostream& stream) const override;

      private:
        void _fetch();
        void move_recurse(boost::filesystem::path const& current,
            boost::filesystem::path const& where);
        friend class Unknown;
        friend class File;
        friend class Symlink;
        friend class Node;
        friend class FileHandle;
        friend std::unique_ptr<Block>
        resolve_directory_conflict(Block& b, model::StoreMode store_mode,
          boost::filesystem::path p,
          FileSystem& owner,
          Operation op,
          FileData fd,
          std::weak_ptr<Directory> wd);
        void _commit(Operation op, bool set_mtime = false);
        void _push_changes(Operation op, bool first_write = false);
        Address _address;
        std::unique_ptr<ACLBlock> _block;
        elle::unordered_map<std::string, FileData> _files;
        bool _inherit_auth; //child nodes inherit this dir's permissions
        boost::posix_time::ptime _last_fetch;
        friend class FileSystem;
    };

    std::unique_ptr<Block>
    resolve_directory_conflict(Block& b, model::StoreMode store_mode,
                               boost::filesystem::path p,
                               FileSystem& owner,
                               Operation op,
                               FileData fd,
                               std::weak_ptr<Directory> wd);
  }
}

#endif
