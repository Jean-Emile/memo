#include <infinit/filesystem/Node.hh>

#include <sys/stat.h> // S_IMFT...

#ifdef INFINIT_WINDOWS
# undef stat
#endif

#include <memory>

#include <elle/serialization/json.hh>

#include <infinit/filesystem/Directory.hh>
#include <infinit/filesystem/umbrella.hh>
#include <infinit/filesystem/Unreachable.hh>
#include <infinit/model/blocks/ACLBlock.hh>
#include <infinit/model/doughnut/ACB.hh>
#include <infinit/model/doughnut/Doughnut.hh>
#include <infinit/model/doughnut/NB.hh>
#include <infinit/model/doughnut/User.hh>
#include <infinit/model/doughnut/consensus/Paxos.hh>

ELLE_LOG_COMPONENT("infinit.filesystem.Node");

namespace infinit
{
  namespace filesystem
  {
    class ACLConflictResolver
      : public model::ConflictResolver
    {
    public:
      ACLConflictResolver(model::Model* model,
                          bool r,
                          bool w,
                          std::string const& key)
        : _model(model)
        , _read(r)
        , _write(w)
        , _userkey(key)
      {}

      ACLConflictResolver(elle::serialization::SerializerIn& s,
                          elle::Version const& v)
      {
        serialize(s, v);
      }

      void
      serialize(elle::serialization::Serializer& s,
                elle::Version const& v) override
      {
        s.serialize("read", this->_read);
        s.serialize("write", this->_write);
        s.serialize("userkey", this->_userkey);
        if (s.in())
        {
          infinit::model::doughnut::Doughnut* model = nullptr;
          const_cast<elle::serialization::Context&>(s.context()).get(model);
          ELLE_ASSERT(model);
          this->_model = model;
        }
      }

      std::string
      description() const override
      {
        std::string permissions = elle::sprintf(
          "%s%s",
          (this->_read ? "r" : ""),
          (this->_write ? "w" : ""));
        if (permissions.empty())
          permissions = "none";
        return elle::sprintf("give %s permissions to \"%s\"",
                             permissions, this->_userkey);
      }

      std::unique_ptr<Block>
      operator() (Block& block,
                  Block& current,
                  model::StoreMode mode) override
      {
        ELLE_TRACE(
          "ACLConflictResolver: replaying set_permissions on new block.");
        std::unique_ptr<model::User> user = this->_model->make_user(
          elle::Buffer(this->_userkey.data(), this->_userkey.size()));
        auto& acl = dynamic_cast<model::blocks::ACLBlock&>(current);
        if (!user)
        {
          for (auto& e: acl.list_permissions(*this->_model))
          {
            if (model::doughnut::short_key_hash(
                dynamic_cast<model::doughnut::User*>(e.user.get())->key())
                  == this->_userkey)
            {
              user = std::move(e.user);
              break;
            }
          }
        }
        if (!user)
        {
          ELLE_WARN("ACL conflict resolution failed: no user found for %s",
                    this->_userkey);
          acl.data(acl.data());
        }
        else
        {
          // Force a change to ensure a version bump
          if (!user->name().empty() && user->name()[0] == '#')
          { // we don't know if this is an user or a group so be careful
            acl.set_permissions(*user, this->_read, this->_write);
            acl.data(acl.data());
          }
          else
          {
            acl.set_permissions(*user, !this->_read, !this->_write);
            acl.set_permissions(*user, this->_read, this->_write);
          }
        }
        return current.clone();
      }

      model::Model* _model;
      bool _read;
      bool _write;
      std::string _userkey;
    };

    static const elle::serialization::Hierarchy<model::ConflictResolver>::
    Register<ACLConflictResolver> _register_dcr("acl");

    static const int gid_start = 61234;
    static const int gid_count = 50;
    static int gid_position = 0;
    static std::vector<
      std::unique_ptr<model::blocks::Block>> acl_save(gid_count);
    static bool acl_preserver = getenv("INFINIT_PRESERVE_ACLS");

    void
    Node::rename(boost::filesystem::path const& where)
    {
      std::string newname = where.filename().string();
      boost::filesystem::path newpath = where.parent_path();
      if (!this->_parent)
        throw rfs::Error(EINVAL, "Cannot delete root node");
      auto destparent = this->_owner.filesystem()->path(newpath.string());
      auto dir = std::dynamic_pointer_cast<Directory>(destparent);
      if (!dir)
      {
        if (std::dynamic_pointer_cast<Unreachable>(destparent))
        {
          THROW_ACCES
        }
        else
        {
          THROW_NOTDIR
        }
      }
      dir->_fetch();
      if (dir->_data->_files.find(newname) != dir->_data->_files.end())
      {
        ELLE_TRACE_SCOPE("%s: remove existing destination", *this);
        // File and empty dir gets removed.
        auto target = this->_owner.filesystem()->path(where.string());
        struct stat st;
        target->stat(&st);
        if (S_ISDIR(st.st_mode))
        {
          try
          {
            target->rmdir();
          }
          catch(rfs::Error const& e)
          {
            throw rfs::Error(EISDIR, "Target is a directory");
          }
        }
        else
          target->unlink();
        ELLE_DEBUG("removed move target %s", where);
      }
      auto data = this->_parent->_files.at(this->_name);

      dir->_data->_files.insert(std::make_pair(newname, data));
      dir->_data->write(
        *this->_owner.block_store(),
        {OperationType::insert, newname, data.first, data.second});

      this->_parent->_files.erase(_name);
      this->_parent->write(*this->_owner.block_store(),
                            {OperationType::remove, this->_name});

      this->_name = newname;
    }

    void
    Node::chmod(mode_t mode)
    {
      this->_fetch();
      auto acl = _header_block();
      this->_header().mode = mode;
      ELLE_DEBUG("chmod setting mode to %x", mode & 0777);
      this->_header().ctime = time(nullptr);
      if (acl)
      {
        auto wm = acl->get_world_permissions();
        wm.first = mode & 4;
        wm.second = mode & 2;
        ELLE_DEBUG("setting world permissions to %s,%s", wm.first, wm.second);
        acl->set_world_permissions(wm.first, wm.second);
      }
      this->_commit(WriteTarget::perms);
    }

    void
    Node::chown(int uid, int gid)
    {
      this->_fetch();
      auto& h = this->_header();
      h.uid = uid;
      h.gid = gid;
      if (acl_preserver && gid >= gid_start && gid < gid_start + gid_count
        && acl_save[gid - gid_start])
      {
        auto block = this->_header_block();
        // clear current perms
        auto perms = block->list_permissions({});
        for (auto const& p: perms)
          try
          { // owner is in that list and we cant touch his perms
            block->set_permissions(*p.user, false, false);
          }
          catch (elle::Error const& e)
          {}
        dynamic_cast<model::blocks::ACLBlock*>(
          acl_save[gid - gid_start].get())->copy_permissions(*block);
      }
      h.ctime = time(nullptr);
      this->_commit(WriteTarget::block);
    }

    void
    Node::removexattr(std::string const& k)
    {
      ELLE_LOG_COMPONENT("infinit.filesystem.Node.xattr");
      ELLE_TRACE_SCOPE("%s: remove attribute \"%s\"", *this, k);
      this->_fetch();
      if (this->_header().xattrs.erase(k))
      {
        this->_header().ctime = time(nullptr);
        this->_commit(WriteTarget::xattrs);
      }
      else
        ELLE_TRACE_SCOPE("no such attribute");
    }

    static auto const overlay_info = "user.infinit.overlay.";

    void
    Node::setxattr(std::string const& k, std::string const& v, int flags)
    {
      ELLE_LOG_COMPONENT("infinit.filesystem.Node.xattr");
      ELLE_TRACE_SCOPE("%s: set attribute \"%s\"", *this, k);
      ELLE_DUMP("value: %s", elle::ConstWeakBuffer(v));
      if (auto special = xattr_special(k))
      {
        auto dht = std::dynamic_pointer_cast<model::doughnut::Doughnut>(
          this->_owner.block_store());
        if (*special == "block.nodes")
        {
          auto ids = elle::serialization::json::deserialize<
            std::unordered_set<model::Address>>(v);
          if (auto paxos = dynamic_cast<model::doughnut::consensus::Paxos*>(
                dht->consensus().get()))
          {
            paxos->rebalance(_address, ids);
            return;
          }
        }
        else if (*special == "block.rebalance")
        {
          if (this->_owner.block_store()->version() < elle::Version(0, 5, 0))
            THROW_NOSYS;
          if (auto paxos = dynamic_cast<model::doughnut::consensus::Paxos*>(
                dht->consensus().get()))
          {
            paxos->rebalance(_address);
            return;
          }
        }
        if (special->find("auth_others") == 0)
        {
          auto block = this->_header_block();
          bool r = v.find("r") != std::string::npos;
          bool w = v.find("w") != std::string::npos;
          umbrella([&] {
              block->set_world_permissions(r, w);
              _commit(WriteTarget::block);
          }, EACCES);
          return;
        }
        throw rfs::Error(ENOATTR, "no such attribute", elle::Backtrace());
      }
      /* Drop quarantine flags, preventing the files from being opened.
      * https://github.com/osxfuse/osxfuse/issues/162
      */
      if (k == "com.apple.quarantine")
        return;
      if (k.substr(0, strlen(overlay_info)) == overlay_info)
      {
        std::string okey = k.substr(strlen(overlay_info));
        umbrella([&] {
          dynamic_cast<model::doughnut::Doughnut*>(
            this->_owner.block_store().get())->overlay()->query(okey, v);
        }, EINVAL);
        return;
      }
      this->_header().xattrs[k] = elle::Buffer(v.data(), v.size());
      this->_header().ctime = time(nullptr);
      this->_commit(WriteTarget::xattrs);
    }

    static
    std::string
    getxattr_block(model::doughnut::Doughnut& dht,
                   std::string const& op,
                   model::Address const& addr)
    {
      if (op == "address")
      {
        return elle::serialization::json::serialize(
          elle::sprintf("%x", addr)).string();
      }
      else if (op == "nodes")
      {
        std::vector<std::string> nodes;
        // FIXME: hardcoded 3
        for (auto n: dht.overlay()->lookup(addr, 3, overlay::OP_FETCH))
        {
          if (auto locked = n.lock())
            nodes.push_back(elle::sprintf("%f", locked->id()));
        }
        std::stringstream s;
        elle::serialization::json::serialize(nodes, s);
        return s.str();
      }
      else if (op == "stat")
      {
        std::stringstream s;
        elle::serialization::json::serialize(
          dht.consensus()->stat(addr), s, false);
        return s.str();
      }
      else
        THROW_INVAL;
    }

    std::string
    Node::getxattr(std::string const& k)
    {
      ELLE_LOG_COMPONENT("infinit.filesystem.Node.xattr");
      ELLE_TRACE_SCOPE("%s: get attribute \"%s\"", *this, k);
      auto dht = std::dynamic_pointer_cast<model::doughnut::Doughnut>(
        this->_owner.block_store());
      if (auto special = xattr_special(k))
      {
        model::blocks::Block* block = this->_header_block();
        if (*special == "block")
        {
          if (!block)
          {
            this->_fetch();
            block = this->_header_block();
            ELLE_ASSERT(block);
          }
          return elle::serialization::json::serialize(block).string();
        }
        else if (special->find("block.") == 0)
        {
          auto op = special->substr(6);
          if (block)
            return getxattr_block(*dht, op, block->address());
          else if (this->_parent)
          {
            auto const& elem = this->_parent->_files.at(this->_name);
            return getxattr_block(*dht, op, elem.second);
          }
          else
            return "<ROOT>";
        }
        else if (special->find("blocks.") == 0)
        {
          auto blocks = special->substr(7);
          auto dot = blocks.find(".");
          if (dot == std::string::npos)
          {
            auto addr = model::Address::from_string(blocks);
            auto block = this->_owner.block_store()->fetch(addr);
            std::stringstream s;
            elle::serialization::json::serialize(block, s);
            return s.str();
          }
          else
          {
            auto addr = model::Address::from_string(blocks.substr(0, dot));
            auto op = blocks.substr(dot + 1);
            return getxattr_block(*dht, op, addr);
          }
        }
        else if (special->find("mountpoint") == 0)
        {
          return (
            this->_owner.mountpoint() ? this->_owner.mountpoint().get().string()
                                      : "");
        }
        else if (special->find("root") == 0)
        {
          return this->full_path() == this->full_path().root_path() ? "true"
                                                                    : "false";
        }
      }
      if (k.substr(0, strlen(overlay_info)) == overlay_info)
      {
        std::string okey = k.substr(strlen(overlay_info));
        elle::json::Json v = umbrella([&] {
          return dynamic_cast<model::doughnut::Doughnut*>(
            this->_owner.block_store().get())->overlay()->query(okey, {});
        }, EINVAL);
        if (v.empty())
          return "{}";
        else
          return elle::json::pretty_print(v);
      }
      auto it = this->_header().xattrs.find(k);
      if (it == this->_header().xattrs.end())
      {
        ELLE_DEBUG("no such attribute");
        throw rfs::Error(ENOATTR, "No attribute", elle::Backtrace());
      }
      std::string value = it->second.string();
      ELLE_DUMP("value: %s", elle::ConstWeakBuffer(value));
      return value;
    }

    void
    Node::stat(struct stat* st)
    {
      memset(st, 0, sizeof(struct stat));
#ifndef INFINIT_WINDOWS
      st->st_blksize = 16384;
      st->st_blocks = this->_header().size / 512;
#endif
      auto h = this->_header();
      st->st_mode  = h.mode;
      st->st_size  = h.size;
      st->st_atime = h.atime;
      st->st_mtime = h.mtime;
      st->st_ctime = h.ctime;
      st->st_nlink = h.links;
      st->st_uid   =
#ifdef INFINIT_WINDOWS
        0;
#else
        getuid();
#endif
      this->_fetch();
      if (!acl_preserver)
        st->st_gid =
#ifdef INFINIT_WINDOWS
        0;
#else
        getgid();
#endif
      else
      {
        auto block = this->_header_block();
        acl_save[gid_position] = block->clone();
        dynamic_cast<model::blocks::MutableBlock*>(
          acl_save[gid_position].get())->data(elle::Buffer());
        st->st_gid = gid_start + gid_position;
        gid_position = (gid_position + 1) % gid_count;
      }
      st->st_dev = 1;
      st->st_ino = (unsigned short)(uint64_t)(void*)this;
      ELLE_DEBUG("%s: stat mode=%x size=%s links=%s",
                 *this, h.mode&0777, h.size, h.links);
    }

    void
    Node::utimens(const struct timespec tv[2])
    {
      ELLE_TRACE_SCOPE("%s: utimens: %s", *this, tv);
      this->_header().atime = tv[0].tv_sec;
      this->_header().mtime = tv[1].tv_sec;
      this->_header().ctime = time(nullptr);
      this->_commit(WriteTarget::times);
    }

    std::unique_ptr<infinit::model::User>
    Node::_get_user(std::string const& value)
    {
      if (value.empty())
        THROW_INVAL;
      ELLE_TRACE("setxattr raw key");
      elle::Buffer userkey = elle::Buffer(value.data(), value.size());
      auto user = this->_owner.block_store()->make_user(userkey);
      return user;
    }

    static std::pair<bool, bool> parse_flags(std::string const& flags)
    {
      bool r = false;
      bool w = false;
      if (flags == "clear")
        ;
      else if (flags == "setr")
        r = true;
      else if (flags == "setw")
        w = true;
      else if (flags == "setrw")
      {
        r = true;
        w = true;
      }
      else
        THROW_NODATA;
      return std::make_pair(r, w);
    }

    void
    Node::set_permissions(std::string const& flags,
                          std::string const& userkey,
                          Address self_address)
    {
      ELLE_TRACE_SCOPE("%s: set_permissions(%s)", *this, flags);
      std::pair<bool, bool> perms = parse_flags(flags);
      auto acl = std::dynamic_pointer_cast<model::blocks::ACLBlock>(
        this->_owner.fetch_or_die(self_address));
      if (!acl)
        throw rfs::Error(EIO, "Block is not an ACL block");
      // permission check
      auto acb = dynamic_cast<model::doughnut::ACB*>(acl.get());
      if (!acb)
      {
#ifdef __clang__
#  pragma GCC diagnostic push
#  pragma GCC diagnostic ignored "-Wpotentially-evaluated-expression"
#endif
        throw rfs::Error(EIO,
          elle::sprintf("Block is not an ACB block: %s", typeid(*acl).name()));
#ifdef __clang__
#  pragma GCC diagnostic pop
#endif
      }
      auto dn = std::dynamic_pointer_cast<model::doughnut::Doughnut>(
        this->_owner.block_store());
      auto keys = dn->keys();
      if (keys.K() != *acb->owner_key())
        THROW_ACCES;
      std::unique_ptr<infinit::model::User> user =
        umbrella([&] {return this->_get_user(userkey);}, EINVAL);
      if (!user)
      {
        if (!userkey.empty() && userkey[0] == '#')
        { // might be a short hash, try to look it up in the block ACLs.
          for (auto& e: acl->list_permissions(*dn))
          {
            if (e.user->name() == userkey
              && model::doughnut::short_key_hash(
                dynamic_cast<model::doughnut::User*>(e.user.get())->key())
                  == userkey)
            {
              user = std::move(e.user);
              break;
            }
          }
        }
        if (!user)
        {
          ELLE_WARN("user %s does not exist", userkey);
          THROW_INVAL;
        }
      }
      ELLE_TRACE("Setting permission at %s for %s",
                 acl->address(), user->name());
      umbrella([&] {acl->set_permissions(*user, perms.first, perms.second);},
        EACCES);
      this->_owner.store_or_die(
        std::move(acl),
        model::STORE_UPDATE,
        elle::make_unique<ACLConflictResolver>(
          this->_owner.block_store().get(), perms.first, perms.second, userkey
        ));
    }

    boost::filesystem::path
    Node::full_path()
    {
      if (this->_parent)
        return this->_parent->_path / this->_name;
      else
        return "/";
    }

    boost::optional<std::string>
    xattr_special(std::string const& name)
    {
      if (name.find("infinit.") == 0)
        return name.substr(8);
      if (name.find("user.infinit.") == 0)
        return name.substr(13);
      return {};
    }
  }
}
