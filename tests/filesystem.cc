#include <dirent.h>
#include <errno.h>
#include <random>

#include <sys/types.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#ifndef INFINIT_WINDOWS
# include <sys/statvfs.h>
#endif

#ifdef INFINIT_WINDOWS
#undef stat
#endif

#ifdef INFINIT_LINUX
# include <attr/xattr.h>
#elif defined(INFINIT_MACOSX)
# include <sys/xattr.h>
#endif

#include <boost/filesystem/fstream.hpp>

#include <elle/UUID.hh>
#include <elle/format/base64.hh>
#include <elle/os/environ.hh>
#include <elle/serialization/Serializer.hh>
#include <elle/serialization/json.hh>
#include <elle/system/Process.hh>
#include <elle/test.hh>
#include <elle/utils.hh>
#include <elle/Version.hh>

#include <reactor/scheduler.hh>

#include <infinit/filesystem/filesystem.hh>
#include <infinit/model/doughnut/Doughnut.hh>
#include <infinit/model/doughnut/Local.hh>
#include <infinit/model/doughnut/Cache.hh>
#include <infinit/model/doughnut/consensus/Paxos.hh>
#include <infinit/model/faith/Faith.hh>
#include <infinit/overlay/Stonehenge.hh>
#include <infinit/storage/Filesystem.hh>
#include <infinit/storage/Memory.hh>
#include <infinit/storage/Storage.hh>
#include <infinit/version.hh>

#include "DHT.hh"

#ifdef INFINIT_MACOSX
# define SXA_EXTRA ,0
#else
# define SXA_EXTRA
#endif

ELLE_LOG_COMPONENT("test");

#define INFINIT_ELLE_VERSION elle::Version(INFINIT_MAJOR,   \
                                           INFINIT_MINOR,   \
                                           INFINIT_SUBMINOR)


namespace ifs = infinit::filesystem;
namespace rfs = reactor::filesystem;
namespace bfs = boost::filesystem;

bool mounted = false;
infinit::storage::Storage* g_storage;
reactor::filesystem::FileSystem* fs;
reactor::Scheduler* sched;

std::vector<std::string> mount_points;
std::vector<std::unique_ptr<infinit::model::doughnut::Doughnut>> nodes;
std::vector<boost::asio::ip::tcp::endpoint> endpoints;
infinit::model::NodeLocations peers;
std::vector<std::unique_ptr<elle::system::Process>> processes;

#ifdef INFINIT_WINDOWS
#define O_CREAT _O_CREAT
#define O_RDWR _O_RDWR
#define O_EXCL _O_EXCL
#define S_IFREG _S_IFREG

int setxattr(const char* path, const char* name, const void* value, int value_size, int)
{
  struct stat st;
  stat(path, &st);
  std::string attrpath;
  if ((st.st_mode & S_IFDIR) || strlen(path) == 2)
    attrpath = std::string(path) + "/" + "$xattrs..";
  else
    attrpath = bfs::path(path).parent_path().string() + "/$xattrs." + bfs::path(path).filename().string();
  attrpath += std::string("/") + name;
  std::ofstream ofs(attrpath);
  ofs.write((const char*)value, value_size);
  std::cerr << "setxattr '" << path << "' " << attrpath << ": " << ofs.good() << std::endl;
  return 0;
}

int setxattr(const wchar_t* path, const char* name, const void* value, int value_size, int)
{
  std::string s;
  for (int i=0; path[i]; ++i)
    s += (char)path[i];
  return setxattr(s.c_str(), name, value, value_size, 0);
}

int stat(const wchar_t* path, struct stat* st)
{
  std::string s;
  for (int i=0; path[i]; ++i)
    s += (char)path[i];
  return stat(s.c_str(), st);
}
int getxattr(const char* path, const char*name, void* buf, int buf_size)
{
  struct stat st;
  stat(path, &st);
  std::string attrpath;
  if ((st.st_mode & S_IFDIR) || strlen(path) == 2)
    attrpath = std::string(path) + "/" + "$xattrs..";
  else
    attrpath = bfs::path(path).parent_path().string() + "/$xattrs." + bfs::path(path).filename().string();
  attrpath += std::string("/") + name;
  std::ifstream ifs(attrpath);
  ifs.read((char*)buf, buf_size);
  std::cerr << "getxattr '" << path << "' " << attrpath << ": " << ifs.good() << std::endl;
  auto gc = ifs.gcount();
  return gc ? gc : -1;
}
int getxattr(const wchar_t* path, const char*name, void* buf, int buf_size)
{
  std::string s;
  for (int i=0; path[i]; ++i)
    s += (char)path[i];
  return getxattr(s.c_str(), name, buf, buf_size);
}

int open(const wchar_t* path, int flags, int mode = 0)
{
  std::string s;
  for (int i=0; path[i]; ++i)
    s += (char)path[i];
  return open(s.c_str(), flags, mode);
}

#endif

static
int
setxattr_(bfs::path p, std::string const& name, std::string const& value)
{
  return setxattr(p.c_str(), name.c_str(), value.c_str(), value.size(),
                  0 SXA_EXTRA);
}

std::string
getxattr_(bfs::path p, std::string const& name)
{
  char buf[2048];
  int res = getxattr(p.c_str(), name.c_str(), buf, 2048 SXA_EXTRA SXA_EXTRA);
  if (res >= 0)
  {
    buf[res] = 0;
    return buf;
  }
  else
    return "";
}

static
int
group_create(bfs::path p, std::string const& name)
{
  return setxattr(p.c_str(), "user.infinit.group.create",
                  name.c_str(), name.size(), 0 SXA_EXTRA);
}

static
int
group_add(bfs::path p, std::string const& gname, std::string const& uname)
{
  std::string cmd = gname + ":" + uname;
  return setxattr(p.c_str(), "user.infinit.group.add",
                  cmd.c_str(), cmd.size(), 0 SXA_EXTRA);
}

static
int
group_remove(bfs::path p, std::string const& gname, std::string const& uname)
{
  std::string cmd = gname + ":" + uname;
  return setxattr(p.c_str(), "user.infinit.group.remove",
                  cmd.c_str(), cmd.size(), 0 SXA_EXTRA);
}

static
int
group_add_admin(bfs::path p, std::string const& gname, std::string const& uname)
{
  std::string cmd = gname + ":" + uname;
  return setxattr(p.c_str(), "user.infinit.group.addadmin",
                  cmd.c_str(), cmd.size(), 0 SXA_EXTRA);
}

static
int
group_remove_admin(
  bfs::path p, std::string const& gname, std::string const& uname)
{
  std::string cmd = gname + ":" + uname;
  return setxattr(p.c_str(), "user.infinit.group.removeadmin",
                  cmd.c_str(), cmd.size(), 0 SXA_EXTRA);
}

static
int
group_delete(bfs::path p, std::string const& gname)
{
  return setxattr(p.c_str(), "user.infinit.group.delete",
                  gname.c_str(), gname.size(), 0 SXA_EXTRA);
}

static
void
wait_for_mounts(
  boost::filesystem::path root, int count, struct statvfs* start = nullptr)
{
  struct statvfs stparent;
#ifndef INFINIT_WINDOWS
  if (start)
  {
    stparent = *start;
    ELLE_LOG("initializing with %s %s", stparent.f_fsid, stparent.f_blocks);
  }
  else
    statvfs(root.string().c_str(), &stparent);
#endif
  while (mount_points.size() < unsigned(count))
    usleep(20000);
#if defined(INFINIT_MACOSX) || defined(INFINIT_WINDOWS)
  // stat change monitoring does not work for unknown reasons
  usleep(2000000);
  return;
#else
  struct statvfs st;
  for (int i=0; i<count; ++i)
  {
    while (true)
    {
      int res = statvfs(mount_points[i].c_str(), &st);
      ELLE_TRACE("%s fsid: %s %s  blk %s %s", i, st.f_fsid, stparent.f_fsid,
                 st.f_blocks, stparent.f_blocks);
      // statvfs failure with EPERM means its mounted
      if (res < 0
        || st.f_fsid != stparent.f_fsid
        || st.f_blocks != stparent.f_blocks
        || st.f_bsize != stparent.f_bsize
        || st.f_flag != stparent.f_flag)
        break;
      usleep(20000);
    }
  }
#endif
}

static
int
directory_count(boost::filesystem::path const& p)
{
  boost::system::error_code erc;
  try
  {
    boost::filesystem::directory_iterator d(p, erc);
    if (erc)
      throw std::runtime_error("construction failed : " + erc.message());
    int s=0;
    while (d != boost::filesystem::directory_iterator())
    {
      ++s;
      d.increment(erc);
      if (erc)
        throw std::runtime_error("increment failed : " + erc.message());
    }
    return s;
  }
  catch (std::exception const& e)
  {
    ELLE_LOG("directory_count failed with %s", e.what());
    return -1;
  }
}

static
bool
can_access(boost::filesystem::path const& p,
           bool read = false, bool read_all = false,
           int expected_errno = EACCES)
{
  struct stat st;
  if (stat(p.string().c_str(), &st) == -1)
  {
    BOOST_CHECK_EQUAL(errno, expected_errno);
    return false;
  }
  if (S_ISDIR(st.st_mode))
  {
    auto dir = opendir(p.string().c_str());
    if (!dir)
      return false;
    else
    {
      auto ent = readdir(dir);
      auto e = errno;
      closedir(dir);
      return ent || e != EACCES;
    }
  }
  else
  {
    int fd = open(p.string().c_str(), O_RDONLY);
    if (fd < 0)
      return false;
    if (read)
    {
      char buf[1024];
      int res = ::read(fd, buf, 1024);
      if (res < 0)
      {
        close(fd);
        return false;
      }
      if (read_all)
      {
        do
        {
          res = ::read(fd, buf, 1024);
          if (res < 0)
          {
            close(fd);
            return false;
          }
        } while (res > 0);
      }
    }
    close(fd);
    return true;
  }
}

static
bool
touch(boost::filesystem::path const& p)
{
  boost::filesystem::ofstream ofs(p);
  if (!ofs.good())
    return false;
  ofs << "test";
  return true;
}

template<typename T>
std::string
serialize(T & t)
{
  elle::Buffer buf;
  {
    elle::IOStream ios(buf.ostreambuf());
    elle::serialization::json::SerializerOut so(ios, false);
    so.serialize_forward(t);
  }
  return buf.string();
}


// Run nodes in a separate scheduler to avoid reentrency issues
// ndmefyl: WHAT THE FUCK is that supposed to imply O.o
reactor::Scheduler* nodes_sched;
static
void
make_nodes(std::string store,
           int node_count,
           infinit::cryptography::rsa::KeyPair const& owner,
           bool paxos)
{
  reactor::Scheduler s;
  nodes_sched = &s;
  reactor::Thread t(s, "nodes", [&] {
    std::vector<infinit::model::Address> ids;
    ids.reserve(node_count);
    for (int i = 0; i < node_count; ++i)
      ids.emplace_back(infinit::model::Address::random(0)); // FIXME
    for (int i = 0; i < node_count; ++i)
    {
      // Create storage
      std::unique_ptr<infinit::storage::Storage> s;
      if (!elle::os::getenv("STORAGE_MEMORY", "").empty())
        s.reset(new infinit::storage::Memory());
      else
      {
        auto tmp = store / boost::filesystem::unique_path();
        std::cerr << i << " : " << tmp << std::endl;
        boost::filesystem::create_directories(tmp);
        s.reset(new infinit::storage::Filesystem(tmp));
      }
      auto kp = infinit::cryptography::rsa::keypair::generate(2048);
      infinit::model::doughnut::Passport passport(kp.K(), "testnet", owner);
      infinit::model::doughnut::Doughnut::ConsensusBuilder consensus =
        [paxos] (infinit::model::doughnut::Doughnut& dht)
        -> std::unique_ptr<infinit::model::doughnut::consensus::Consensus>
        {
          if (paxos)
            return elle::make_unique<
              infinit::model::doughnut::consensus::Paxos>(dht, 3);
          else
            return elle::make_unique<
              infinit::model::doughnut::consensus::Consensus>(dht);
        };
      infinit::model::doughnut::Doughnut::OverlayBuilder overlay =
        [=] (infinit::model::doughnut::Doughnut& dht,
             std::shared_ptr<infinit::model::doughnut::Local> local)
        {
          auto res = elle::make_unique<infinit::overlay::Stonehenge>(
            infinit::model::NodeLocations(), std::move(local), &dht);
          return res;
         };
      nodes.emplace_back(
        new infinit::model::doughnut::Doughnut(
          ids[i],
          std::make_shared<infinit::cryptography::rsa::KeyPair>(kp),
          owner.public_key(),
          passport,
          consensus,
          overlay,
          boost::optional<int>(),
          std::move(s),
          INFINIT_ELLE_VERSION));
    }
    for (int i = 0; i < node_count; ++i)
      peers.emplace_back(
        ids[i],
        infinit::model::Endpoints(
          {{"127.0.0.1", nodes[i]->local()->server_endpoint().port()}}));
    for (auto const& node: nodes)
      elle::unconst(static_cast<infinit::overlay::Stonehenge*>(
                      node->overlay().get())->peers()) = peers;
  });
  ELLE_LOG("Running node scheduler");
  s.run();
  ELLE_LOG("Exiting node scheduler");
}

static
void
run_filesystem_dht(std::vector<infinit::cryptography::rsa::PublicKey>& keys,
                   std::string const& store,
                   std::string const& mountpoint,
                   int node_count,
                   int nread = 1,
                   int nwrite = 1,
                   int nmount = 1,
                   bool paxos = true)
{
  sched = new reactor::Scheduler();
  fs = nullptr;
  mount_points.clear();
  nodes.clear();
  endpoints.clear();
  processes.clear();
  peers.clear();
  mounted = false;
  auto owner_keys = infinit::cryptography::rsa::keypair::generate(2048);
  new std::thread([&] { make_nodes(store, node_count, owner_keys, paxos);});
  while (peers.size() != unsigned(node_count))
    usleep(100000);
  ELLE_TRACE("got %s nodes, preparing %s mounts", nodes.size(), nmount);
  std::vector<reactor::Thread*> threads;
  reactor::Thread t(*sched, "fs", [&] {
    mount_points.reserve(nmount);
    for (int i=0; i< nmount; ++i)
    {
      std::string mp = mountpoint;
      if (nmount != 1)
      {
        mp = (mp / boost::filesystem::unique_path()).string();
      }
#ifdef INFINIT_WINDOWS
      mp.clear();
      mp += ('t' + i);
      mp += ':';
#endif
      mount_points.push_back(mp);
      boost::system::error_code erc;
      boost::filesystem::create_directories(mp, erc);
      if (nmount == 1)
      {
        ELLE_TRACE("configuring mounter...");
        //auto kp = infinit::cryptography::rsa::keypair::generate(2048);
        //keys.push_back(kp.K());
        keys.emplace_back(owner_keys.K());
        infinit::model::doughnut::Passport passport(
          owner_keys.K(), "testnet", owner_keys);
        ELLE_TRACE("instantiating dougnut...");
        infinit::model::doughnut::Doughnut::ConsensusBuilder consensus =
          [paxos] (infinit::model::doughnut::Doughnut& dht)
          -> std::unique_ptr<infinit::model::doughnut::consensus::Consensus>
          {
            std::unique_ptr<infinit::model::doughnut::consensus::Consensus>
            consensus;
            if (paxos)
              consensus = elle::make_unique<
                infinit::model::doughnut::consensus::Paxos>(dht, 3);
            else
              consensus = elle::make_unique<
                infinit::model::doughnut::consensus::Consensus>(dht);
            consensus = elle::make_unique<
              infinit::model::doughnut::consensus::Cache>
                (std::move(consensus), 1000);
            return consensus;
          };
        infinit::model::doughnut::Doughnut::OverlayBuilder overlay =
          [=] (infinit::model::doughnut::Doughnut& dht,
               std::shared_ptr<infinit::model::doughnut::Local> local)
          {
            ELLE_DEBUG("Instanciating stonehenge with %s peers", peers.size());
            auto res = elle::make_unique<infinit::overlay::Stonehenge>(
              peers, std::move(local), &dht);
            return res;
          };
        std::unique_ptr<infinit::model::Model> model =
        elle::make_unique<infinit::model::doughnut::Doughnut>(
          infinit::model::Address::random(0), // FIXME
          "testnet",
          std::make_shared<infinit::cryptography::rsa::KeyPair>(owner_keys),
          owner_keys.public_key(),
          passport,
          consensus,
          overlay,
          boost::optional<int>(),
          nullptr,
          INFINIT_ELLE_VERSION);
        ELLE_TRACE("instantiating ops...");
        std::unique_ptr<ifs::FileSystem> ops;
        ops = elle::make_unique<ifs::FileSystem>(
          "default-volume", std::move(model), ifs::allow_root_creation = true);
        ELLE_TRACE("instantiating fs...");
        fs = new reactor::filesystem::FileSystem(std::move(ops), true);
        ELLE_TRACE("running mounter...");
        new reactor::Thread("mounter", [mp] {
          ELLE_LOG("mounting on %s", mp);
          mounted = true;
          std::vector<std::string> mount_options = {"", "-o", "hard_remove"};  // {"", "-d" /*, "-o", "use_ino"*/});
#ifdef INFINIT_MACOSX
          mount_options.push_back("-o");
          mount_options.push_back("nobrowse");
#endif
          fs->mount(mp, mount_options);
          ELLE_TRACE("waiting...");
          reactor::wait(*fs);
          ELLE_TRACE("...done");
#ifndef INFINIT_MACOSX
          ELLE_LOG("filesystem unmounted");
          nodes_sched->mt_run<void>("clearer", [] { nodes.clear();});
          processes.clear();
#endif
          reactor::scheduler().terminate();
        });
      }
      else
      {
        // Having more than one mount in the same process is failing
        // Make a config file.
        elle::json::Object r;
        r["single_mount"] = false;
        r["mountpoint"] = mp;
        elle::json::Object model;
        model["type"] = "doughnut";
        model["name"] = "user" + std::to_string(i);
        auto kp = infinit::cryptography::rsa::keypair::generate(2048);
        keys.emplace_back(kp.K());
        model["id"] = elle::format::base64::encode(
          elle::ConstWeakBuffer(
            infinit::model::Address::random(0).value(), // FIXME
            sizeof(infinit::model::Address::Value))).string();
        model["keys"] = "@KEYS@"; // placeholder, lolilol
        model["passport"] = "@PASSPORT@"; // placeholder, lolilol
        model["owner"] = "@OWNER@"; // placeholder, lolilol
        {
          elle::json::Object consensus;
          if (paxos)
          {
            consensus["type"] = "paxos";
            consensus["replication-factor"] = 3;
          }
          else
          {
            consensus["type"] = "single";
          }
          model["consensus"] = std::move(consensus);
        }
        {
          elle::json::Object overlay;
          overlay["type"] = "stonehenge";
          elle::json::Array v;
          for (auto const& p: peers)
          {
            elle::json::Object po;
            po["id"] = elle::format::base64::encode(
              elle::ConstWeakBuffer(
                p.id().value(),
                sizeof(infinit::model::Address::Value))).string();
            ELLE_ASSERT_GT(p.endpoints().size(), 0u);
            po["host"] = p.endpoints()[0].address().to_string();
            po["port"] = p.endpoints()[0].port();
            v.push_back(po);
          }
          overlay["peers"] = v;
          model["overlay"] = std::move(overlay);
          model["version"] =
            elle::sprintf("%s.%s", INFINIT_MAJOR, INFINIT_MINOR);
        }
        r["model"] = model;
        std::string kps;
        if (i == 0)
          kps = serialize(owner_keys);
        else
          kps = serialize(kp);
        std::string owner_ser = serialize(owner_keys.K());
        infinit::model::doughnut::Passport passport(
          i == 0 ? owner_keys.K() : kp.K(), "testnet", owner_keys);
        std::string passport_ser = serialize(passport);
        std::stringstream ss;
        elle::json::write(ss, r, true);
        std::string ser = ss.str();
        // Now replace placeholder with key
        size_t pos = ser.find("\"@KEYS@\"");
        ser = ser.substr(0, pos) + kps + ser.substr(pos + 8);
        pos = ser.find("\"@PASSPORT@\"");
        ser = ser.substr(0, pos) + passport_ser + ser.substr(pos + 12);
        pos = ser.find("\"@OWNER@\"");
        ser = ser.substr(0, pos) + owner_ser + ser.substr(pos + 9);
        {
          std::ofstream ofs(mountpoint + "/" + std::to_string(i));
          ofs.write(ser.data(), ser.size());
        }
        std::vector<std::string> args {
          elle::sprintf("%s/bin/infinit", elle::os::getenv("BUILD_DIR", ".")),
          "-c",
          (mountpoint + "/" + std::to_string(i))
        };
        processes.emplace_back(new elle::system::Process(args));
        reactor::sleep(1_sec);
      }
    }
  });
  ELLE_TRACE("sched running");
  sched->run();
  ELLE_TRACE("sched exiting");
#ifdef INFINIT_MACOSX
  if (nmount == 1)
  {
    ELLE_LOG("filesystem unmounted");
    nodes_sched->mt_run<void>("clearer", [] { nodes.clear();});
    processes.clear();
  }
#endif
}

static
void
run_filesystem(std::string const& store, std::string const& mountpoint)
{
  sched = new reactor::Scheduler();
  fs = nullptr;
  mount_points.clear();
  nodes.clear();
  endpoints.clear();
  processes.clear();
  mounted = false;
  auto tmp = boost::filesystem::temp_directory_path()
           / boost::filesystem::unique_path();
  std::unique_ptr<infinit::model::Model> model;
  reactor::Thread t(*sched, "fs", [&] {
    if (!elle::os::getenv("STORAGE_MEMORY", "").empty())
      storage = new infinit::storage::Memory();
    else
      storage = new infinit::storage::Filesystem(store);
    model = elle::make_unique<infinit::model::faith::Faith>(
      std::unique_ptr<infinit::storage::Storage>(g_storage),
      INFINIT_ELLE_VERSION);
    std::unique_ptr<ifs::FileSystem> ops = elle::make_unique<ifs::FileSystem>(
      "default-volume", std::move(model), ifs::allow_root_creation = true);
    fs = new reactor::filesystem::FileSystem(std::move(ops), true);
    mount_points.push_back(mountpoint);
    mounted = true;
    fs->mount(mountpoint, {"", "-ohard_remove"}); // {"", "-d" /*, "-o", "use_ino"*/});
    reactor::wait(*fs);
  });
  sched->run();
}

static
std::string
read(boost::filesystem::path const& where)
{
  std::string text;
  boost::filesystem::ifstream ifs(where);
  ifs >> text;
  return text;
}

static
void
read_all(boost::filesystem::path const& where)
{
  boost::filesystem::ifstream ifs(where);
  char buffer[1024];
  while (true)
  {
    ifs.read(buffer, 1024);
    if (!ifs.gcount())
      return;
  }
}

static
void
write(boost::filesystem::path const& where, std::string const& what)
{
  boost::filesystem::ofstream ofs(where);
  ofs << what;
}

void
test_filesystem(bool dht,
                int nnodes = 5,
                int nread = 1,
                int nwrite = 1,
                bool paxos = true)
{
  namespace bfs = boost::filesystem;
  auto store = boost::filesystem::temp_directory_path()
             / boost::filesystem::unique_path();
  auto mount = boost::filesystem::temp_directory_path()
             / boost::filesystem::unique_path();
  elle::os::setenv("INFINIT_HOME", store.string(), true);
  boost::filesystem::create_directories(mount);
  boost::filesystem::create_directories(store);
  mount_points.clear();
  struct statvfs statstart;
#ifndef INFINIT_WINDOWS
  statvfs(mount.string().c_str(), &statstart);
#else
  mount = "t:";
#endif
  std::vector<infinit::cryptography::rsa::PublicKey> keys;
  std::thread t([&] {
      if (dht)
        run_filesystem_dht(keys, store.string(), mount.string(), 5,
                           nread, nwrite, 1, paxos);
      else
        run_filesystem(store.string(), mount.string());
    });
  wait_for_mounts(mount, 1, &statstart);
#ifdef INFINIT_WINDOWS
  Sleep(15000);
#endif
  ELLE_LOG("starting test, mnt=%s, store=%s", mount, store);

  elle::SafeFinally remover([&] {
      ELLE_TRACE("unmounting");
      //fs->unmount();
      try
      {
        sched->mt_run<void>("unmounter", [&] { fs->unmount();});
      }
      catch(std::exception const& e)
      {
        ELLE_TRACE("unmounter threw %s", e.what());
      }
      //reactor::Thread th(*sched, "unmount", [&] { fs->unmount();});
      t.join();
      ELLE_TRACE("cleaning up");
      for (auto const& mp: mount_points)
      {
        std::vector<std::string> args
#ifdef INFINIT_MACOSX
          {"umount", "-f", mp};
#else
          {"fusermount", "-u", mp};
#endif
        elle::system::Process p(args);
      }
      try
      {
        usleep(200000);
        boost::filesystem::remove_all(store);
        ELLE_TRACE("remove mount");
        boost::filesystem::remove_all(mount);
        ELLE_TRACE("Cleaning done");
      }
      catch (std::exception const& e)
      {
        ELLE_TRACE("Exception cleaning up: %s", e.what());
      }
  });
  std::string text;
  {
    boost::filesystem::ofstream ofs(mount / "test");
    ofs << "Test";
  }
  BOOST_CHECK_EQUAL(directory_count(mount), 1);
  {
    bfs::ifstream ifs(mount / "test");
    ifs >> text;
  }
  BOOST_CHECK_EQUAL(text, "Test");
  {
    bfs::ofstream ofs(mount / "test",
                      std::ofstream::out|std::ofstream::ate|std::ofstream::app);
    ofs << "coin";
  }
  BOOST_CHECK_EQUAL(directory_count(mount), 1);
  {
    bfs::ifstream ifs(mount / "test");
    ifs >> text;
  }
  BOOST_CHECK_EQUAL(text, "Testcoin");
  BOOST_CHECK_EQUAL(bfs::file_size(mount/"test"), 8);
  bfs::remove(mount / "test");
  BOOST_CHECK_EQUAL(directory_count(mount), 0);
  boost::system::error_code erc;
  bfs::file_size(mount / "foo", erc);
  BOOST_CHECK_EQUAL(true, !!erc);

  ELLE_LOG("truncate")
  {
    char buffer[16384];
    ELLE_LOG("write massive file")
    {
      bfs::ofstream ofs(mount / "tt");
      for (int i=0; i<100; ++i)
        ofs.write(buffer, 16384);
    }
    int tfd = open( (mount / "tt").c_str(), O_RDWR);
    ELLE_LOG("truncate file");
    int tres = ftruncate(tfd, 0);
    if (tres)
      perror("ftruncate");
    BOOST_CHECK_EQUAL(tres, 0);
#ifndef INFINIT_WINDOWS
    // FIXME: ftruncate is translated to dokany call SetEndOfFile() on the file,
    // so the opened file handle is not notified
    ELLE_LOG("successive writes")
    {
      BOOST_CHECK_EQUAL(write(tfd, buffer, 16384), 16384);;
      BOOST_CHECK_EQUAL(write(tfd, buffer, 12288), 12288);
      BOOST_CHECK_EQUAL(write(tfd, buffer, 3742), 3742);
    }
    ELLE_LOG("truncate file")
    {
      BOOST_CHECK_EQUAL(ftruncate(tfd, 32414), 0);
      BOOST_CHECK_EQUAL(ftruncate(tfd, 32413), 0);
    }
    close(tfd);
    ELLE_LOG("check file size")
      BOOST_CHECK_EQUAL(bfs::file_size(mount / "tt"), 32413);
#else
    close(tfd);
#endif
    bfs::remove(mount / "tt");
  }

  // hardlink
#ifdef INFINIT_LINUX
  struct stat st;
  {
    bfs::ofstream ofs(mount / "test");
    ofs << "Test";
  }
  bfs::create_hard_link(mount / "test", mount / "test2");
  {
    bfs::ofstream ofs(mount / "test2",
                      std::ofstream::out |
                      std::ofstream::ate |
                      std::ofstream::app);
    ofs << "coinB";
    ofs.close();
  }
  usleep(500000);
  stat((mount / "test").string().c_str(), &st);
  BOOST_CHECK_EQUAL(st.st_size, 9);
  stat((mount / "test2").string().c_str(), &st);
  BOOST_CHECK_EQUAL(st.st_size, 9);
  text = read(mount / "test2");
  BOOST_CHECK_EQUAL(text, "TestcoinB");
  text = read(mount / "test");
  BOOST_CHECK_EQUAL(text, "TestcoinB");

  {
    bfs::ofstream ofs(mount / "test",
                      std::ofstream::out |
                      std::ofstream::ate |
                      std::ofstream::app);
    ofs << "coinA";
  }
  // XXX [@Matthieu]: Should be 500000.
  usleep(750000);
  stat((mount / "test").string().c_str(), &st);
  BOOST_CHECK_EQUAL(st.st_size, 14);
  stat((mount / "test2").string().c_str(), &st);
  BOOST_CHECK_EQUAL(st.st_size, 14);
  text = read(mount / "test");
  BOOST_CHECK_EQUAL(text, "TestcoinBcoinA");
  text = read(mount / "test2");
  BOOST_CHECK_EQUAL(text, "TestcoinBcoinA");
  bfs::remove(mount / "test");
  text = read(mount / "test2");
  BOOST_CHECK_EQUAL(text, "TestcoinBcoinA");
  bfs::remove(mount / "test2");

  // hardlink opened handle
  {
    bfs::ofstream ofs(mount / "test");
    ofs << "Test";
  }
  {
    bfs::ofstream ofs(mount / "test",
                      std::ofstream::out |
                      std::ofstream::ate |
                      std::ofstream::app);
    ofs << "a";
    bfs::create_hard_link(mount / "test", mount / "test2");
    ofs << "b";
    ofs.close();
    text = read(mount / "test");
    BOOST_CHECK_EQUAL(text, "Testab");
    text = read(mount / "test2");
    BOOST_CHECK_EQUAL(text, "Testab");
    bfs::remove(mount / "test");
    bfs::remove(mount / "test2");
  }
#endif

  //holes
  int fd = open((mount / "test").string().c_str(), O_RDWR|O_CREAT, 0644);
  if (fd < 0)
    perror("open");
  ELLE_ENFORCE_EQ(write(fd, "foo", 3), 3);
  lseek(fd, 10, SEEK_CUR);
  ELLE_ENFORCE_EQ(write(fd, "foo", 3), 3);
  close(fd);
  {
    bfs::ifstream ifs(mount / "test");
    char buffer[20];
    ifs.read(buffer, 20);
    BOOST_CHECK_EQUAL(ifs.gcount(), 16);
    char expect[] = {'f','o','o',0,0,0,0,0,0,0,0,0,0,'f','o','o'};
    BOOST_CHECK_EQUAL(std::string(buffer, buffer + 16),
                      std::string(expect, expect + 16));
  }
  bfs::remove(mount / "test");

  ELLE_LOG("test use after unlink")
  {
    fd = open((mount / "test").string().c_str(), O_RDWR|O_CREAT, 0644);
    if (fd < 0)
      perror("open");
    ELLE_LOG("write initial data")
      ELLE_ENFORCE_EQ(write(fd, "foo", 3), 3);
    ELLE_LOG("unlink")
      bfs::remove(mount / "test");
    ELLE_LOG("write additional data")
    {
      int res = write(fd, "foo", 3);
      BOOST_CHECK_EQUAL(res, 3);
    }
    ELLE_LOG("reread data")
    {
      lseek(fd, 0, SEEK_SET);
      char buf[7] = {0};
      int res = read(fd, buf, 6);
      BOOST_CHECK_EQUAL(res, 6);
      BOOST_CHECK_EQUAL(buf, "foofoo");
    }
    close(fd);
    BOOST_CHECK_EQUAL(directory_count(mount), 0);
  }

  ELLE_LOG("test rename")
  {
    {
      boost::filesystem::ofstream ofs(mount / "test");
      ofs << "Test";
    }
    bfs::rename(mount / "test", mount / "test2");
    BOOST_CHECK_EQUAL(read(mount / "test2"), "Test");
    write(mount / "test3", "foo");
    bfs::rename(mount / "test2", mount / "test3");
    BOOST_CHECK_EQUAL(read(mount / "test3"), "Test");
    BOOST_CHECK_EQUAL(directory_count(mount), 1);
    bfs::create_directory(mount / "dir");
    write(mount / "dir" / "foo", "bar");
    bfs::rename(mount / "test3", mount / "dir", erc);
    BOOST_CHECK_EQUAL(!!erc, true);
    bfs::rename(mount / "dir", mount / "dir2");
    bfs::remove(mount / "dir2", erc);
    BOOST_CHECK_EQUAL(!!erc, true);
    bfs::rename(mount / "dir2" / "foo", mount / "foo");
    bfs::remove(mount / "dir2");
    bfs::remove(mount / "foo");
    bfs::remove(mount / "test3");
  }

  ELLE_LOG("test cross-block")
  {
    struct stat st;
    int fd = open((mount / "babar").string().c_str(), O_RDWR|O_CREAT, 0644);
    BOOST_CHECK_GE(fd, 0);
    lseek(fd, 1024*1024 - 10, SEEK_SET);
    const char* data = "abcdefghijklmnopqrstuvwxyz";
    int res = write(fd, data, strlen(data));
    BOOST_CHECK_EQUAL(res, strlen(data));
    close(fd);
    stat((mount / "babar").string().c_str(), &st);
    BOOST_CHECK_EQUAL(st.st_size, 1024 * 1024 - 10 + 26);
    char output[36];
    fd = open((mount / "babar").string().c_str(), O_RDONLY);
    BOOST_CHECK_GE(fd, 0);
    lseek(fd, 1024*1024 - 15, SEEK_SET);
    res = read(fd, output, 36);
    BOOST_CHECK_EQUAL(31, res);
    BOOST_CHECK_EQUAL(std::string(output+5, output+31),
                      data);
    BOOST_CHECK_EQUAL(std::string(output, output+31),
                      std::string(5, 0) + data);
    close(fd);
    bfs::remove(mount / "babar");
  }
  ELLE_LOG("test cross-block 2")
  {
    struct stat st;
    int fd = open((mount / "bibar").string().c_str(), O_RDWR|O_CREAT, 0644);
    BOOST_CHECK_GE(fd, 0);
    lseek(fd, 1024*1024 + 16384 - 10, SEEK_SET);
    const char* data = "abcdefghijklmnopqrstuvwxyz";
    int res = write(fd, data, strlen(data));
    BOOST_CHECK_EQUAL(res, strlen(data));
    close(fd);
    stat((mount / "bibar").string().c_str(), &st);
    BOOST_CHECK_EQUAL(st.st_size, 1024 * 1024 +16384 - 10 + 26);
    char output[36];
    fd = open((mount / "bibar").string().c_str(), O_RDONLY);
    BOOST_CHECK_GE(fd, 0);
    lseek(fd, 1024*1024 +16384 - 15, SEEK_SET);
    res = read(fd, output, 36);
    BOOST_CHECK_EQUAL(31, res);
    BOOST_CHECK_EQUAL(std::string(output+5, output+31),
                      data);
    BOOST_CHECK_EQUAL(std::string(output, output+31),
                      std::string(5, 0) + data);
    close(fd);
    bfs::remove(mount / "bibar");
  }

  ELLE_LOG("test link/unlink")
  {
    int fd = open((mount / "u").string().c_str(), O_RDWR|O_CREAT, 0644);
    ::close(fd);
    bfs::remove(mount / "u");
  }

  ELLE_LOG("test multiple open, but with only one open")
  {
    {
      boost::filesystem::ofstream ofs(mount / "test");
      ofs << "Test";
    }
    BOOST_CHECK_EQUAL(read(mount / "test"), "Test");
    bfs::remove(mount / "test");
  }

  ELLE_LOG("test multiple opens")
  {
    {
      boost::filesystem::ofstream ofs(mount / "test");
      ofs << "Test";
      boost::filesystem::ofstream ofs2(mount / "test");
    }
    BOOST_CHECK_EQUAL(read(mount / "test"), "Test");
    bfs::remove(mount / "test");
    {
      boost::filesystem::ofstream ofs(mount / "test");
      ofs << "Test";
      {
        boost::filesystem::ofstream ofs2(mount / "test");
      }
      ofs << "Test";
    }
    BOOST_CHECK_EQUAL(read(mount / "test"), "TestTest");
    bfs::remove(mount / "test");
  }

  ELLE_LOG("test randomizing a file");
  {
    // randomized manyops
    std::default_random_engine gen;
    std::uniform_int_distribution<>dist(0, 255);
    {
      boost::filesystem::ofstream ofs(mount / "tbig", std::ios::binary);
      for (int i=0; i<10000000; ++i)
        ofs.put(dist(gen));
    }
    usleep(1000000);
    ELLE_TRACE("random writes");
    BOOST_CHECK_EQUAL(boost::filesystem::file_size(mount / "tbig"), 10000000);
    std::uniform_int_distribution<>dist2(0, 9999999);
    for (int i=0; i < (dht?1:10); ++i)
    {
      if (! (i%10))
        ELLE_TRACE("Run %s", i);
      ELLE_TRACE("opening");
      int fd = open((mount / "tbig").string().c_str(), O_RDWR);
      for (int i=0; i < 5; ++i)
      {
        int sv = dist2(gen);
        lseek(fd, sv, SEEK_SET);
        unsigned char c = dist(gen);
        ELLE_TRACE("Write 1 at %s", sv);
        BOOST_CHECK_EQUAL(write(fd, &c, 1), 1);
      }
      ELLE_TRACE("Closing");
      close(fd);
    }
    ELLE_TRACE("truncates");
    BOOST_CHECK_EQUAL(boost::filesystem::file_size(mount / "tbig"), 10000000);
  }

  ELLE_LOG("test truncate")
  {
    ELLE_TRACE("truncate 9");
    boost::filesystem::resize_file(mount / "tbig", 9000000);
    read_all(mount / "tbig");
    ELLE_TRACE("truncate 8");
    boost::filesystem::resize_file(mount / "tbig", 8000000);
    read_all(mount / "tbig");
    ELLE_TRACE("truncate 5");
    boost::filesystem::resize_file(mount / "tbig", 5000000);
    read_all(mount / "tbig");
    ELLE_TRACE("truncate 2");
    boost::filesystem::resize_file(mount / "tbig", 2000000);
    read_all(mount / "tbig");
    ELLE_TRACE("truncate .9");
    boost::filesystem::resize_file(mount / "tbig", 900000);
    read_all(mount / "tbig");
    bfs::remove(mount / "tbig");
  }

  ELLE_LOG("test extended attributes")
  {
    setxattr(mount.c_str(), "testattr", "foo", 3, 0 SXA_EXTRA);
    touch(mount / "file");
    setxattr((mount / "file").c_str(), "testattr", "foo", 3, 0 SXA_EXTRA);
    char attrlist[1024];
    ssize_t sz;
#ifndef INFINIT_WINDOWS
    sz = listxattr(mount.c_str(), attrlist, 1024 SXA_EXTRA);
    BOOST_CHECK_EQUAL(sz, strlen("testattr")+1);
    BOOST_CHECK_EQUAL(attrlist, "testattr");
    sz = listxattr( (mount / "file").c_str(), attrlist, 1024 SXA_EXTRA);
    BOOST_CHECK_EQUAL(sz, strlen("testattr")+1);
    BOOST_CHECK_EQUAL(attrlist, "testattr");
    sz = getxattr(mount.c_str(), "testattr", attrlist,
                  1024 SXA_EXTRA SXA_EXTRA);
#endif
    BOOST_CHECK_EQUAL(sz, strlen("foo"));
    attrlist[sz] = 0;
    BOOST_CHECK_EQUAL(attrlist, "foo");
    sz = getxattr( (mount / "file").c_str(), "testattr", attrlist,
                  1024 SXA_EXTRA SXA_EXTRA);
    BOOST_CHECK_EQUAL(sz, strlen("foo"));
    attrlist[sz] = 0;
    BOOST_CHECK_EQUAL(attrlist, "foo");
    sz = getxattr( (mount / "file").c_str(), "nope", attrlist,
                  1024 SXA_EXTRA SXA_EXTRA);
    BOOST_CHECK_EQUAL(sz, -1);
    sz = getxattr( (mount / "nope").c_str(), "nope", attrlist,
                  1024 SXA_EXTRA SXA_EXTRA);
    BOOST_CHECK_EQUAL(sz, -1);
    sz = getxattr( mount.c_str(), "nope", attrlist, 1024 SXA_EXTRA SXA_EXTRA);
    BOOST_CHECK_EQUAL(sz, -1);
    bfs::remove(mount / "file");
  }
  ELLE_LOG("simultaneus read/write");
  {
    bfs::ofstream ofs(mount / "test");
    char buf[1024];
    // write enough data so that the read will cause a cache eviction
    for (int i=0; i< 22 * 1024; ++i)
      ofs.write(buf, 1024);
    bfs::ifstream ifs(mount / "test");
    ifs.read(buf, 1024);
    ofs.write(buf, 1024);
    ifs.close();
    ofs.close();
    bfs::remove(mount / "test");
  }

  ELLE_LOG("test symlink")
  {
    auto real_path = mount / "real_file";
    auto symlink_path = mount / "symlink";
    ELLE_TRACE("write real file")
    {
      bfs::ofstream ofs(real_path);
      ofs << "something";
    }
    ELLE_TRACE("create symlink")
    {
      bfs::create_symlink(real_path, symlink_path);
    }
    std::string text;
    ELLE_TRACE("read through symlink")
    {
      bfs::ifstream ifs(symlink_path);
      ifs >> text;
    }
    BOOST_CHECK_EQUAL(text, "something");
    bfs::remove(real_path);
    bfs::remove(symlink_path);
  }

  ELLE_LOG("utf-8")
  {
    const char* name = "éùßñЂ";
    write(mount / name, "foo");
    BOOST_CHECK_EQUAL(read(mount / name), "foo");
    BOOST_CHECK_EQUAL(directory_count(mount), 1);
    bfs::directory_iterator it(mount);
    BOOST_CHECK_EQUAL(it->path().filename(), name);
    BOOST_CHECK_EQUAL(it->path().filename(), std::string(name));
    bfs::remove(mount / name);
    BOOST_CHECK_EQUAL(directory_count(mount), 0);
  }
}

void
test_basic()
{
  test_filesystem(false);
}

void
filesystem()
{
  test_filesystem(true, 5, 1, 1, false);
}

void
filesystem_paxos()
{
  test_filesystem(true, 5, 1, 1, true);
}

void
unmounter(boost::filesystem::path mount,
          boost::filesystem::path store,
          std::thread& t)
{
  ELLE_LOG("unmounting");
  if (!nodes_sched->done())
    nodes_sched->mt_run<void>("clearer", [] { nodes.clear();});
  ELLE_LOG("cleaning up: TERM %s", processes.size());
#ifndef INFINIT_WINDOWS
  for (auto const& p: processes)
    kill(p->pid(), SIGTERM);
  usleep(200000);
  ELLE_LOG("cleaning up: KILL");
  for (auto const& p: processes)
    kill(p->pid(), SIGKILL);
  usleep(200000);
#endif
  // unmount all
  for (auto const& mp: mount_points)
  {
    std::vector<std::string> args
#ifdef INFINIT_MACOSX
    {"umount", mp};
#else
    {"fusermount", "-u", mp};
#endif
    elle::system::Process p(args);
  }
  usleep(200000);
  boost::filesystem::remove_all(mount);
  boost::filesystem::remove_all(store);
  t.join();
  ELLE_LOG("teardown complete");
}

void
test_conflicts(bool paxos)
{
  namespace bfs = boost::filesystem;
  auto store = bfs::temp_directory_path() / bfs::unique_path();
  auto mount = bfs::temp_directory_path() / bfs::unique_path();
  elle::os::setenv("INFINIT_HOME", store.string(), true);
  bfs::create_directories(mount);
  bfs::create_directories(store);
  struct statvfs statstart;
#ifndef INFINIT_WINDOWS
  statvfs(mount.string().c_str(), &statstart);
#endif
  mount_points.clear();
  std::vector<infinit::cryptography::rsa::PublicKey> keys;
  std::thread t([&] {
      run_filesystem_dht(keys, store.string(), mount.string(),
                                5, 1, 1, 2, paxos);
  });
  wait_for_mounts(mount, 2, &statstart);
  elle::SafeFinally remover([&] {
      try
      {
        unmounter(mount, store, t);
      }
      catch (std::exception const& e)
      {
        ELLE_TRACE("unmounter threw %s", e.what());
      }
  });
  // Mounts/keys are in mount_points and keys
  // First entry got the root!
  BOOST_CHECK_EQUAL(mount_points.size(), 2);
  bfs::path m0 = mount_points[0];
  bfs::path m1 = mount_points[1];
  BOOST_CHECK_EQUAL(keys.size(), 2);
  std::string k1 = serialize(keys[1]);
  ELLE_LOG("set permissions");
  {
    setxattr(m0.c_str(), "user.infinit.auth.setrw",
             k1.c_str(), k1.length(), 0 SXA_EXTRA);
    setxattr(m0.c_str(), "user.infinit.auth.inherit",
           "true", strlen("true"), 0 SXA_EXTRA);
  }
  ELLE_LOG("file create/write conflict")
  {
    int fd0, fd1;
    ELLE_LOG("open file 0")
      fd0 = open((m0 / "file").string().c_str(), O_CREAT|O_RDWR, 0644);
    BOOST_CHECK(fd0 != -1);
    ELLE_LOG("open file 1")
      fd1 = open((m1 / "file").string().c_str(), O_CREAT|O_RDWR, 0644);
    BOOST_CHECK(fd1 != -1);
    ELLE_LOG("write to file 0")
      BOOST_CHECK_EQUAL(write(fd0, "foo", 3), 3);
    ELLE_LOG("write to file 1")
      BOOST_CHECK_EQUAL(write(fd1, "bar", 3), 3);
    ELLE_LOG("close file 0")
      BOOST_CHECK_EQUAL(close(fd0), 0);
    ::usleep(2000000);
    ELLE_LOG("close file 1")
      BOOST_CHECK_EQUAL(close(fd1), 0);
    ELLE_LOG("read file 0")
      BOOST_CHECK_EQUAL(read(m0/"file"), "bar");
    ELLE_LOG("read file 1")
      BOOST_CHECK_EQUAL(read(m1/"file"), "bar");
  }
  // FIXME: This needs cache to be enabled ; restore when cache is moved up to
  // Model instead of the consensus and the 'infinit' binary accepts --cache.
  // ELLE_LOG("file create/write without acl inheritance")
  // {
  //   int fd0, fd1;
  //   setxattr(m0.c_str(), "user.infinit.auth.inherit",
  //            "false", strlen("false"), 0 SXA_EXTRA);
  //   // Force caching of '/' in second mount, otherwise if it fetches,
  //   // it will see the new 'file2' and fail the create.
  //   struct stat st;
  //   stat(m1.string().c_str(), &st);
  //   fd0 = open((m0 / "file2").string().c_str(), O_CREAT|O_RDWR, 0644);
  //   BOOST_CHECK(fd0 != -1);
  //   fd1 = open((m1 / "file2").string().c_str(), O_CREAT|O_RDWR, 0644);
  //   BOOST_CHECK(fd1 != -1);
  //   BOOST_CHECK_EQUAL(write(fd0, "foo", 3), 3);
  //   BOOST_CHECK_EQUAL(write(fd1, "bar", 3), 3);
  //   BOOST_CHECK_EQUAL(close(fd0), 0);
  //   BOOST_CHECK_EQUAL(close(fd1), 0);
  //   BOOST_CHECK_EQUAL(read(m1/"file"), "bar");
  // }
  struct stat st;
  ELLE_LOG("directory conflict")
  {
    int fd0, fd1;
    // force file node into filesystem cache
    stat((m0/"file3").c_str(), &st);
    stat((m1/"file4").c_str(), &st);
    fd0 = open((m0 / "file3").string().c_str(), O_CREAT|O_RDWR, 0644);
    BOOST_CHECK(fd0 != -1);
    fd1 = open((m1 / "file4").string().c_str(), O_CREAT|O_RDWR, 0644);
    BOOST_CHECK(fd1 != -1);
    BOOST_CHECK_EQUAL(close(fd0), 0);
    BOOST_CHECK_EQUAL(close(fd1), 0);
    BOOST_CHECK_EQUAL(stat((m0/"file3").c_str(), &st), 0);
    BOOST_CHECK_EQUAL(stat((m1/"file4").c_str(), &st), 0);
  }
  ELLE_LOG("write/replace")
  {
    int fd0, fd1;
    fd0 = open((m0 / "file6").string().c_str(), O_CREAT|O_RDWR, 0644);
    BOOST_CHECK(fd0 != -1);
    BOOST_CHECK_EQUAL(write(fd0, "coin", 4), 4);
    BOOST_CHECK_EQUAL(close(fd0), 0);
    fd1 = open((m1 / "file6bis").string().c_str(), O_CREAT|O_RDWR, 0644);
    BOOST_CHECK(fd1 != -1);
    BOOST_CHECK_EQUAL(write(fd1, "nioc", 4), 4);
    BOOST_CHECK_EQUAL(close(fd1), 0);
    fd0 = open((m0 / "file6").string().c_str(), O_CREAT|O_RDWR|O_APPEND, 0644);
    BOOST_CHECK(fd0 != -1);
    ELLE_LOG("write");
    BOOST_CHECK_EQUAL(write(fd0, "coin", 4), 4);
    ELLE_LOG("rename");
    bfs::rename(m1 / "file6bis", m1 / "file6");
    ELLE_LOG("close");
    BOOST_CHECK_EQUAL(close(fd0), 0);
    BOOST_CHECK_EQUAL(read(m0/"file6"), "nioc");
    BOOST_CHECK_EQUAL(read(m1/"file6"), "nioc");
  }
  ELLE_LOG("create O_EXCL");
  {
    int fd0 = open((m0 / "file7").string().c_str(), O_CREAT | O_EXCL | O_RDWR, 0644);
    BOOST_CHECK(fd0 != -1);
    int fd1 = open((m1 / "file7").string().c_str(), O_CREAT | O_EXCL | O_RDWR, 0644);
    int err = errno;
    BOOST_CHECK_EQUAL(fd1, -1);
    BOOST_CHECK_EQUAL(err, EEXIST);
    close(fd0);
    close(fd1);
  }
}

void
conflicts()
{
  test_conflicts(false);
}

void
conflicts_paxos()
{
  test_conflicts(true);
}

std::vector<infinit::model::Address>
get_fat(std::string const& attr)
{
  std::stringstream input(attr);
  std::vector<infinit::model::Address> res;
  for (auto const& entry:
         boost::any_cast<elle::json::Array>(elle::json::read(input)))
    res.push_back(infinit::model::Address::from_string(
                    boost::any_cast<std::string>(entry)));
  return res;
}

std::vector<infinit::model::Address>
get_fat(boost::filesystem::path const& path)
{
  return get_fat(getxattr_(path, "user.infinit.fat"));
}

static
void
test_acl(bool paxos)
{
  namespace bfs = boost::filesystem;
  boost::system::error_code erc;
  auto store = bfs::temp_directory_path() / bfs::unique_path();
  auto mount = bfs::temp_directory_path() / bfs::unique_path();
  elle::os::setenv("INFINIT_HOME", store.string(), true);
  bfs::create_directories(mount);
  bfs::create_directories(store);
  struct statvfs statstart;
#ifndef INFINIT_WINDOWS
  statvfs(mount.string().c_str(), &statstart);
#endif
  mount_points.clear();
  std::vector<infinit::cryptography::rsa::PublicKey> keys;
  std::thread t([&] {
      run_filesystem_dht(keys, store.string(), mount.string(),
                         5, 1, 1, 2, paxos);
  });
  wait_for_mounts(mount, 2, &statstart);
  ELLE_LOG("Test start");
  elle::SafeFinally remover([&] {
    try
    {
      unmounter(mount, store, t);
    }
    catch (std::exception const& e)
    {
      ELLE_TRACE("unmounter threw %s", e.what());
    }
  });
  // Mounts/keys are in mount_points and keys
  // First entry got the root!
  BOOST_CHECK_EQUAL(mount_points.size(), 2);
  bfs::path m0 = mount_points[0];
  bfs::path m1 = mount_points[1];
  //bfs::path m2 = mount_points[2];
  BOOST_CHECK_EQUAL(keys.size(), 2);
  std::string k1 = serialize(keys[1]);
  {
    boost::filesystem::ofstream ofs(m0 / "test");
    ofs << "Test";
  }
  BOOST_CHECK_EQUAL(directory_count(m0), 1);
  BOOST_CHECK_EQUAL(directory_count(m1), -1);
  BOOST_CHECK(!can_access(m1/"test"));
  {
     boost::filesystem::ifstream ifs(m1 / "test");
     BOOST_CHECK_EQUAL(ifs.good(), false);
  }
  setxattr(m0.c_str(), "user.infinit.auth.setrw",
    k1.c_str(), k1.length(), 0 SXA_EXTRA);
  // expire directory cache
  usleep(1100000);
  // k1 can now list directory
  BOOST_CHECK_EQUAL(directory_count(m1), 1);
  // but the file is still not readable
  BOOST_CHECK(!can_access(m1/"test"));
  boost::filesystem::remove(m1 / "test", erc);
  BOOST_CHECK(erc);
  BOOST_CHECK_EQUAL(directory_count(m1), 1);
  BOOST_CHECK_EQUAL(directory_count(m0), 1);
  setxattr((m0/"test").c_str(), "user.infinit.auth.setrw",
    k1.c_str(), k1.length(), 0 SXA_EXTRA);
  usleep(1100000);
  BOOST_CHECK(can_access(m1/"test"));
  {
     boost::filesystem::ifstream ifs(m1 / "test");
     BOOST_CHECK_EQUAL(ifs.good(), true);
     std::string v;
     ifs >> v;
     BOOST_CHECK_EQUAL(v, "Test");
  }
  setxattr((m0/"test").c_str(), "user.infinit.auth.clear",
    k1.c_str(), k1.length(), 0 SXA_EXTRA);
  BOOST_CHECK(!can_access(m1/"test"));
  setxattr((m0/"test").c_str(), "user.infinit.auth.setrw",
    k1.c_str(), k1.length(), 0 SXA_EXTRA);
  BOOST_CHECK(can_access(m1/"test"));
  bfs::create_directory(m0 / "dir1");
  BOOST_CHECK(touch(m0 / "dir1" / "pan"));
  BOOST_CHECK(!can_access(m1 / "dir1"));
  BOOST_CHECK(!can_access(m1 / "dir1" / "pan"));
  BOOST_CHECK(!touch(m1 / "dir1" / "coin"));
  setxattr((m0 / "dir1").c_str(), "user.infinit.auth.setrw",
    k1.c_str(), k1.length(), 0 SXA_EXTRA);
  BOOST_CHECK(can_access(m1 / "dir1"));
  BOOST_CHECK(!can_access(m1 / "dir1" / "pan"));
  BOOST_CHECK(touch(m1 / "dir1" / "coin"));
  // test by user name
  touch(m0 / "byuser");
  BOOST_CHECK(!can_access(m1 / "byuser"));
  setxattr((m0 / "byuser").c_str(), "user.infinit.auth.setrw",
    "user1", strlen("user1"), 0 SXA_EXTRA);
  BOOST_CHECK(can_access(m1/"test"));
  BOOST_CHECK(can_access(m1 / "byuser"));
  // inheritance
  bfs::create_directory(m0 / "dirs");
  ELLE_LOG("setattrs");
  setxattr((m0 / "dirs").c_str(), "user.infinit.auth.setrw",
    k1.c_str(), k1.length(), 0 SXA_EXTRA);
  BOOST_CHECK_EQUAL(directory_count(m1 / "dirs"), 0);
  ELLE_LOG("setinherit");
  setxattr((m0 / "dirs").c_str(), "user.infinit.auth.inherit",
    "true", strlen("true"), 0 SXA_EXTRA);
  ELLE_LOG("create childs");
  touch(m0 / "dirs" / "coin");
  bfs::create_directory(m0 / "dirs" / "dir");
  touch(m0 / "dirs" / "dir" / "coin");
  BOOST_CHECK(can_access(m1 / "dirs" / "coin"));
  BOOST_CHECK(can_access(m1 / "dirs" / "dir" / "coin"));
  BOOST_CHECK_EQUAL(directory_count(m1 / "dirs"), 2);
  // readonly
  bfs::create_directory(m0 / "dir2");
  setxattr((m0 / "dir2").c_str(), "user.infinit.auth.setr",
    k1.c_str(), k1.length(), 0 SXA_EXTRA);
  BOOST_CHECK(touch(m0 / "dir2" / "coin"));
  setxattr((m0 / "dir2"/ "coin").c_str(), "user.infinit.auth.setr",
    k1.c_str(), k1.length(), 0 SXA_EXTRA);
  BOOST_CHECK(can_access(m1 / "dir2" / "coin"));
  BOOST_CHECK(!touch(m1 / "dir2" / "coin"));
  BOOST_CHECK(!touch(m1 / "dir2" / "pan"));

  ELLE_LOG("world-readable");
  setxattr((m0).c_str(), "user.infinit.auth.inherit",
    "false", strlen("false"), 0 SXA_EXTRA);
  bfs::create_directory(m0 / "dir3");
  bfs::create_directory(m0 / "dir3" / "dir");
  {
     boost::filesystem::ofstream ofs(m0 / "dir3" / "file");
     ofs << "foo";
  }
  BOOST_CHECK_EQUAL(directory_count(m0 / "dir3"), 2);
  BOOST_CHECK_EQUAL(directory_count(m1 / "dir3"), -1);
  // open dir3
  bfs::permissions(m0 / "dir3", bfs::add_perms | bfs::others_read);
  BOOST_CHECK_EQUAL(directory_count(m1 / "dir3"), 2);
  bfs::create_directory(m1 / "dir3" / "tdir", erc);
  BOOST_CHECK(erc);
  BOOST_CHECK_EQUAL(directory_count(m1 / "dir3" / "dir"), -1);
  // close dir3
  bfs::permissions(m0 / "dir3", bfs::remove_perms | bfs::others_read);
  BOOST_CHECK_EQUAL(directory_count(m1 / "dir3"), -1);
  bfs::permissions(m0 / "dir3", bfs::add_perms | bfs::others_read);
  bfs::permissions(m0 / "dir3" / "file", bfs::add_perms | bfs::others_read);
  bfs::permissions(m0 / "dir3" / "dir", bfs::add_perms | bfs::others_read);
  BOOST_CHECK_EQUAL(directory_count(m1 / "dir3" / "dir"), 0);
  BOOST_CHECK_EQUAL(read(m1 / "dir3" / "file"), "foo");
  write(m1 / "dir3" / "file", "babar");
  BOOST_CHECK_EQUAL(read(m1 / "dir3" / "file"), "foo");
  BOOST_CHECK_EQUAL(read(m0 / "dir3" / "file"), "foo");
  write(m0 / "dir3" / "file", "bim");
  BOOST_CHECK_EQUAL(read(m1 / "dir3" / "file"), "bim");
  BOOST_CHECK_EQUAL(read(m0 / "dir3" / "file"), "bim");
  bfs::create_directory(m0 / "dir3" / "dir2");
  BOOST_CHECK_EQUAL(directory_count(m1 / "dir3"), 3);
  BOOST_CHECK_EQUAL(directory_count(m0 / "dir3"), 3);
  bfs::permissions(m0 / "dir3" / "file", bfs::remove_perms | bfs::others_read);
  write(m0 / "dir3" / "file", "foo2");
  BOOST_CHECK_EQUAL(read(m0 / "dir3" / "file"), "foo2");

  ELLE_LOG("world-writable");
  bfs::create_directory(m0 / "dir4");
  BOOST_CHECK_EQUAL(directory_count(m1 / "dir4"), -1);
  bfs::permissions(m0 / "dir4",
                   bfs::add_perms |bfs::others_write | bfs::others_read);
  BOOST_CHECK_EQUAL(directory_count(m1 / "dir4"), 0);
  write(m1 / "dir4" / "file", "foo");
  bfs::create_directory(m1 /"dir4"/ "dir");
  BOOST_CHECK_EQUAL(read(m0 / "dir4" / "file"), "");
  BOOST_CHECK_EQUAL(read(m1 / "dir4" / "file"), "foo");
  BOOST_CHECK_EQUAL(directory_count(m0 / "dir4" / "dir"), -1);
  BOOST_CHECK_EQUAL(directory_count(m1 / "dir4" / "dir"), 0);
  bfs::permissions(m0 / "dir4", bfs::remove_perms |bfs::others_write);
  BOOST_CHECK_EQUAL(read(m1 / "dir4" / "file"), "foo");

  write(m0 / "file5", "foo");
  bfs::permissions(m0 / "file5",
                   bfs::add_perms |bfs::others_write | bfs::others_read);
  write(m1 / "file5", "bar");
  BOOST_CHECK_EQUAL(read(m1 / "file5"), "bar");
  BOOST_CHECK_EQUAL(read(m0 / "file5"), "bar");
  bfs::permissions(m0 / "file5", bfs::remove_perms |bfs::others_write);
  write(m1 / "file5", "barbar");
  BOOST_CHECK_EQUAL(read(m1 / "file5"), "bar");
  BOOST_CHECK_EQUAL(read(m0 / "file5"), "bar");

  ELLE_LOG("groups");
  write(m0 / "g1", "foo");
  BOOST_CHECK_EQUAL(read(m0 / "g1"), "foo");
  BOOST_CHECK_EQUAL(read(m1 / "g1"), "");
  group_create(m0, "group1");
  group_add(m0, "group1", "user1");
  setxattr((m0 / "g1").c_str(), "user.infinit.auth.setrw",
    "@group1", 7, 0 SXA_EXTRA);
  usleep(1100000);
  BOOST_CHECK_EQUAL(read(m1 / "g1"), "foo");

  group_remove(m0, "group1", "user1");
  write(m0 / "g2", "foo");
  setxattr((m0 / "g2").c_str(), "user.infinit.auth.setrw",
    "@group1", 7, 0 SXA_EXTRA);
  BOOST_CHECK_EQUAL(read(m1 / "g2"), "");
  group_add(m0, "group1", "user1");
  usleep(1100000);
  BOOST_CHECK_EQUAL(read(m1 / "g2"), "foo");
  write(m1 / "g2", "bar");
  // now block is signed with group key
  usleep(1100000);
  BOOST_CHECK_EQUAL(read(m0 / "g2"), "bar");
  BOOST_CHECK_EQUAL(read(m1 / "g2"), "bar");
  // force a group update
  group_remove(m0, "group1", "user1");
  usleep(1100000);
  // check we can still fetch stuff
  BOOST_CHECK_EQUAL(read(m0 / "g2"), "bar");
  group_add(m0, "group1", "user1");
  BOOST_CHECK_EQUAL(read(m0 / "g2"), "bar");
  BOOST_CHECK_EQUAL(read(m1 / "g2"), "bar");

  ELLE_LOG("group admin");
  BOOST_CHECK_EQUAL(group_add_admin(m0, "group1", "user1"), 0);
  BOOST_CHECK_EQUAL(group_add(m1, "group1", "user0"), 0);
  write(m1 / "g3", "bar");
  BOOST_CHECK_EQUAL(read(m0 / "g3"), "");
  setxattr((m1 / "g3").c_str(), "user.infinit.auth.setrw",
    "@group1", 7, 0 SXA_EXTRA);
  usleep(1100000);
  BOOST_CHECK_EQUAL(read(m0 / "g3"), "bar");
  BOOST_CHECK_EQUAL(group_remove_admin(m0, "group1", "user1"), 0);
  BOOST_CHECK_EQUAL(group_remove(m1, "group1", "user0"), -1);
  BOOST_CHECK_EQUAL(group_remove_admin(m1, "group1", "user0"), -1);

  //incorrect stuff, check it doesn't crash us
  ELLE_LOG("groups bad operations");
  BOOST_CHECK_EQUAL(group_add(m1, "group1", "user0"), -1);
  BOOST_CHECK_EQUAL(group_create(m0, "group1"), -1);
  BOOST_CHECK_EQUAL(read(m0 / "g1"), "foo");
  group_remove(m0, "group1", "user1");
  group_remove(m0, "group1", "user1");
  BOOST_CHECK_EQUAL(group_add(m0, "group1", "group1"), -1);
  BOOST_CHECK_EQUAL(read(m0 / "g1"), "foo");
  BOOST_CHECK_EQUAL(group_add(m0, "nosuch", "user1"), -1);
  BOOST_CHECK_EQUAL(read(m0 / "g1"), "foo");
  BOOST_CHECK_EQUAL(group_add(m0, "group1","nosuch"), -1);
  BOOST_CHECK_EQUAL(read(m0 / "g1"), "foo");
  BOOST_CHECK_EQUAL(group_remove(m0, "group1","nosuch"), -1);
  BOOST_CHECK_EQUAL(read(m0 / "g1"), "foo");
  BOOST_CHECK_EQUAL(read(m0 / "g1"), "foo");
  BOOST_CHECK_EQUAL(read(m0 / "g1"), "foo");
  BOOST_CHECK_EQUAL(group_delete(m0, "group1"), 0);

  ELLE_LOG("removal");
  //test the xattrs we'll use
  auto base0 = m0 / "dirrm";
  auto base1 = m1 / "dirrm";
  bfs::create_directory(base0);
  setxattr_(base0, "user.infinit.auth.setrw", "user1");
  write(base0 / "rm", "pan");
  BOOST_CHECK_EQUAL(directory_count(base0), 1);
  BOOST_CHECK_EQUAL(directory_count(base1), 1);
  BOOST_CHECK(can_access(base0 / "rm"));
  std::string block = getxattr_(base0 / "rm", "user.infinit.block.address");
  block = block.substr(3, block.size()-5);
  BOOST_CHECK_EQUAL(setxattr_(base0, "user.infinit.fsck.rmblock", block), 0);
  BOOST_CHECK(!can_access(base0 / "rm", true, false, EIO));
  BOOST_CHECK_EQUAL(directory_count(base0), 1);
  setxattr_(base0, "user.infinit.fsck.unlink", "rm");
  BOOST_CHECK_EQUAL(directory_count(base0), 0);

  write(base0 / "rm2", "foo");
  BOOST_CHECK_EQUAL(directory_count(base0), 1);
  block = getxattr_(base0 / "rm2", "user.infinit.block.address");
  block = block.substr(3, block.size()-5);
  BOOST_CHECK_EQUAL(setxattr_(base1, "user.infinit.fsck.rmblock", block), -1);
  BOOST_CHECK(can_access(base0 / "rm2", true));
  bfs::remove(base0 / "rm2");

  ELLE_LOG("removal CHB");
  {
    bfs::ofstream ofs(base0 / "rm3");
    char buffer[16384];
    for (int i=0; i<100; ++i)
      ofs.write(buffer, 16384);
  }
  auto fat = get_fat(base0 / "rm3");
  BOOST_CHECK_EQUAL(setxattr_(base1, "user.infinit.fsck.rmblock",
                              elle::sprintf("%x", fat[0])), -1);
  BOOST_CHECK(can_access(base0 / "rm3", true, true));
  BOOST_CHECK_EQUAL(setxattr_(base0, "user.infinit.fsck.rmblock",
                              elle::sprintf("%x", fat[0])), 0);
  BOOST_CHECK(!can_access(base0 / "rm3", true, true));
  ELLE_LOG("test end");
}

static
void
acl()
{
  test_acl(false);
}

static
void
acl_paxos()
{
  test_acl(true);
}

class NoCheatConsensus: public infinit::model::doughnut::consensus::Consensus
{
public:
  typedef infinit::model::doughnut::consensus::Consensus Super;
  NoCheatConsensus(std::unique_ptr<Super> backend)
  : Super(backend->doughnut())
  , _backend(std::move(backend))
  {}
protected:
  virtual
  std::unique_ptr<infinit::model::blocks::Block>
  _fetch(infinit::model::Address address, boost::optional<int> local_version)
  {
    auto res = _backend->fetch(address, local_version);
    if (!res)
      return res;
    elle::Buffer buf;
    {
      elle::IOStream os(buf.ostreambuf());
      elle::serialization::binary::serialize(res, os);
    }
    elle::IOStream is(buf.istreambuf());
    elle::serialization::Context ctx;
    ctx.set(&doughnut());
    res = elle::serialization::binary::deserialize<std::unique_ptr<blocks::Block>>(
      is, true, ctx);
    return res;
  }
  virtual
  void
  _remove(infinit::model::Address address, infinit::model::blocks::RemoveSignature rs)
  {
    if (rs.block)
    {
      elle::Buffer buf;
      {
        elle::IOStream os(buf.ostreambuf());
        elle::serialization::binary::serialize(rs.block, os);
      }
      elle::IOStream is(buf.istreambuf());
      elle::serialization::Context ctx;
      ctx.set(&doughnut());
      auto res = elle::serialization::binary::deserialize<std::unique_ptr<blocks::Block>>(
        is, true, ctx);
      rs.block = std::move(res);
    }
    _backend->remove(address, rs);
  }
  std::unique_ptr<Super> _backend;
};

std::unique_ptr<infinit::model::doughnut::consensus::Consensus>
no_cheat_consensus(std::unique_ptr<infinit::model::doughnut::consensus::Consensus> c)
{
  return elle::make_unique<NoCheatConsensus>(std::move(c));
}

std::unique_ptr<infinit::model::doughnut::consensus::Consensus>
same_consensus(std::unique_ptr<infinit::model::doughnut::consensus::Consensus> c)
{
  return c;
}

class DHTs
{
public:
  template <typename ... Args>
  DHTs(int count, Args ... args)
    : owner_keys(infinit::cryptography::rsa::keypair::generate(512))
    , dhts()
  {
    pax = true;
    if (count < 0)
    {
      pax = false;
      count *= -1;
    }
    for (int i = 0; i < count; ++i)
    {
      this->dhts.emplace_back(paxos = pax,
                              owner = this->owner_keys,
                              std::forward<Args>(args) ...);
      for (int j = 0; j < i; ++j)
        this->dhts[j].overlay->connect(*this->dhts[i].overlay);
    }
  }

  struct Client
  {
    Client(std::string const& name, DHT dht)
      : dht(std::move(dht))
      , fs(elle::make_unique<reactor::filesystem::FileSystem>(
             elle::make_unique<infinit::filesystem::FileSystem>(
               name, this->dht.dht, ifs::allow_root_creation = true),
             true))
    {}

    DHT dht;
    std::unique_ptr<reactor::filesystem::FileSystem> fs;
  };

  Client
  client(bool new_key = false)
  {
    DHT client(owner = this->owner_keys,
               keys = new_key ? infinit::cryptography::rsa::keypair::generate(512)
                                : this->owner_keys,
               storage = nullptr,
               make_consensus = pax ? same_consensus : no_cheat_consensus,
               paxos = pax);
    for (auto& dht: this->dhts)
      dht.overlay->connect(*client.overlay);
    return Client("volume", std::move(client));
  }

  infinit::cryptography::rsa::KeyPair owner_keys;
  std::vector<DHT> dhts;
  bool pax;
};

ELLE_TEST_SCHEDULED(write_truncate)
{
  DHTs servers(1);
  auto client = servers.client();
  // the emacs save procedure: open() truncate() write()
  auto handle =
    client.fs->path("/file")->create(O_CREAT | O_RDWR, S_IFREG | 0644);
  handle->write(elle::ConstWeakBuffer("foo\nbar\nbaz\n", 12), 12, 0);
  handle->close();
  handle.reset();
  handle = client.fs->path("/file")->open(O_RDWR, 0);
  BOOST_CHECK(handle);
  client.fs->path("/file")->truncate(0);
  handle->write(elle::ConstWeakBuffer("foo\nbar\n", 8), 8, 0);
  handle->close();
  handle.reset();
  struct stat st;
  client.fs->path("/file")->stat(&st);
  BOOST_CHECK_EQUAL(st.st_size, 8);
  handle = client.fs->path("/file")->open(O_RDWR, 0);
  char buffer[64];
  int count = handle->read(elle::WeakBuffer(buffer, 64), 64, 0);
  BOOST_CHECK_EQUAL(count, 8);
  buffer[count] = 0;
  BOOST_CHECK_EQUAL(buffer, std::string("foo\nbar\n"));
}


ELLE_TEST_SCHEDULED(write_unlink)
{
  DHTs servers(1);
  auto client_1 = servers.client();
  auto client_2 = servers.client();
  auto root_1 = [&]
    {
      ELLE_LOG_SCOPE("fetch client 1 root");
      return client_1.fs->path("/");
    }();
  auto root_2 = [&]
    {
      ELLE_LOG_SCOPE("fetch client 2 root");
      return client_2.fs->path("/");
    }();
  auto handle = [&]
  {
    ELLE_LOG_SCOPE("create file file client 1");
    auto handle =
      root_1->child("file")->create(O_CREAT | O_RDWR, S_IFREG | 0644);
    BOOST_CHECK(handle);
    BOOST_CHECK_EQUAL(handle->write(elle::ConstWeakBuffer("data1"), 5, 0), 5);
    return handle;
  }();
  ELLE_LOG("sync on client 1")
    handle->fsync(true);
  struct stat st;
  ELLE_LOG("check file exists on client 1")
    BOOST_CHECK_NO_THROW(root_1->child("file")->stat(&st));
  ELLE_LOG("check file exists on client 2")
    BOOST_CHECK_NO_THROW(root_2->child("file")->stat(&st));
  ELLE_LOG("read on client 1")
  {
    elle::Buffer b(5);
    BOOST_CHECK_EQUAL(handle->read(b, 5, 0), 5);
    BOOST_CHECK_EQUAL(b, "data1");
  }
  ELLE_LOG("remove file on client 2")
    root_2->child("file")->unlink();
  // Ability to read removed files not implemented yet, but it should
  // ELLE_LOG("read on client 1")
  // {
  //   elle::Buffer b(5);
  //   BOOST_CHECK_EQUAL(handle->read(b, 5, 0), 5);
  //   BOOST_CHECK_EQUAL(b, "data1");
  // }
  ELLE_LOG("write on client 1")
    BOOST_CHECK_EQUAL(handle->write(elle::ConstWeakBuffer("data2"), 5, 0), 5);
  ELLE_LOG("sync on client 1")
    handle->fsync(true);
  // ELLE_LOG("read on client 1")
  // {
  //   elle::Buffer b(5);
  //   BOOST_CHECK_EQUAL(handle->read(b, 5, 0), 5);
  //   BOOST_CHECK_EQUAL(b, "data2");
  // }
  ELLE_LOG("close file on client 1")
    BOOST_CHECK_NO_THROW(handle->close());
  ELLE_LOG("check file does not exist on client 2")
    BOOST_CHECK_THROW(root_1->child("file")->stat(&st), elle::Error);
  ELLE_LOG("check file does not exist on client 2")
    BOOST_CHECK_THROW(root_2->child("file")->stat(&st), elle::Error);
}

ELLE_TEST_SCHEDULED(prefetcher_failure)
{
  DHTs servers(1);
  auto client = servers.client();
  ::Overlay* o = dynamic_cast< ::Overlay*>(client.dht.dht->overlay().get());
  auto root = client.fs->path("/");
  BOOST_CHECK(o);
  auto h = root->child("file")->create(O_CREAT | O_RDWR, S_IFREG | 0644);
  // grow to 2 data blocks
  char buf[16384];
  for (int i=0; i<1024*3; ++i)
    h->write(elle::ConstWeakBuffer(buf, 1024), 1024,  1024*i);
  h->close();
  auto fat = get_fat(root->child("file")->getxattr("user.infinit.fat"));
  BOOST_CHECK_EQUAL(fat.size(), 3);
  o->fail_addresses().insert(fat[1]);
  o->fail_addresses().insert(fat[2]);
  auto handle = root->child("file")->open(O_RDWR, 0);
  BOOST_CHECK_EQUAL(handle->read(elle::WeakBuffer(buf, 16384), 16384, 8192),
                    16384);
  reactor::sleep(200_ms);
  o->fail_addresses().clear();
  BOOST_CHECK_EQUAL(
    handle->read(elle::WeakBuffer(buf, 16384), 16384, 1024 * 1024 + 8192),
    16384);
  BOOST_CHECK_EQUAL(
    handle->read(elle::WeakBuffer(buf, 16384), 16384, 1024 * 1024 * 2 + 8192),
    16384);
}

ELLE_TEST_SCHEDULED(paxos_race)
{
  DHTs servers(1);
  auto c1 = servers.client();
  auto c2 = servers.client();
  auto r1 = c1.fs->path("/");
  auto r2 = c2.fs->path("/");
  ELLE_LOG("create both directories")
  {
    reactor::Thread t1("t1", [&] { r1->child("foo")->mkdir(0700);});
    reactor::Thread t2("t2", [&] { r2->child("bar")->mkdir(0700);});
    reactor::wait({t1, t2});
  }
  ELLE_LOG("check")
  {
    int count = 0;
    c1.fs->path("/")->list_directory(
      [&](std::string const&, struct stat*) { ++count;});
    BOOST_CHECK_EQUAL(count, 4);
    count = 0;
    c2.fs->path("/")->list_directory(
      [&](std::string const&, struct stat*) { ++count;});
    BOOST_CHECK_EQUAL(count, 4);
  }
}

ELLE_TEST_SCHEDULED(data_embed)
{
  DHTs servers(1);
  auto client = servers.client();
  auto root = client.fs->path("/");
  auto h = root->child("file")->create(O_CREAT | O_RDWR, S_IFREG | 0644);
  h->write(elle::ConstWeakBuffer("foo", 3), 3, 0);
  h->close();
  h.reset();
  BOOST_CHECK_EQUAL(
    get_fat(root->child("file")->getxattr("user.infinit.fat")).size(),
    0);

  h = root->child("file")->open(O_RDWR, 0);
  h->write(elle::ConstWeakBuffer("foo", 3), 3, 3);
  h->close();
  h.reset();
  BOOST_CHECK_EQUAL(
    get_fat(root->child("file")->getxattr("user.infinit.fat")).size(),
    0);

  h = root->child("file")->open(O_RDWR, 0);
  char buf[1024] = {0};
  BOOST_CHECK_EQUAL(h->read(elle::WeakBuffer(buf, 64), 64, 0), 6);
  BOOST_CHECK_EQUAL(buf, std::string("foofoo"));
  h->close();
  h.reset();

  h = root->child("file")->open(O_RDWR, 0);
  h->write(elle::ConstWeakBuffer("barbarbaz", 9), 9, 0);
  h->close();
  h.reset();
  BOOST_CHECK_EQUAL(
    get_fat(root->child("file")->getxattr("user.infinit.fat")).size(),
    0);
    h = root->child("file")->open(O_RDWR, 0);
  BOOST_CHECK_EQUAL(h->read(elle::WeakBuffer(buf, 64), 64, 0), 9);
  BOOST_CHECK_EQUAL(buf, std::string("barbarbaz"));
  h->close();
  h.reset();

  h = root->child("file")->open(O_RDWR, 0);
  for (int i = 0; i < 1024; ++i)
    h->write(elle::ConstWeakBuffer(buf, 1024), 1024, 1024*i);
  h->close();
  h.reset();
  BOOST_CHECK_EQUAL(
    get_fat(root->child("file")->getxattr("user.infinit.fat")).size(),
    1);

  h = root->child("file2")->create(O_CREAT | O_RDWR, S_IFREG | 0644);
  h->write(elle::ConstWeakBuffer(buf, 1024), 1024, 0);
  for (int i = 0; i < 1024; ++i)
    h->write(elle::ConstWeakBuffer(buf, 1024), 1024, 1024*i);
  h->write(elle::ConstWeakBuffer(buf, 1024), 1024, 1024*1024);
  h->close();
  h.reset();
  BOOST_CHECK_EQUAL(
    get_fat(root->child("file2")->getxattr("user.infinit.fat")).size(),
    2);
}

ELLE_TEST_SCHEDULED(symlink_perms)
{
  // If we enable paxos, it will cache blocks and feed them back to use.
  // Since we use the Locals dirrectly(no remote), there is no
  // serialization at all when fetching, which means we end up with
  // already decyphered blocks
  DHTs servers(-1);
  auto client1 = servers.client(false);
  auto client2 = servers.client(true);
  ELLE_LOG("create file");
  auto h = client1.fs->path("/foo")->create(O_RDWR |O_CREAT, S_IFREG | 0600);
  ELLE_LOG("write file");
  h->write(elle::ConstWeakBuffer("foo", 3), 3, 0);
  h->close();
  h.reset();
  ELLE_LOG("create symlink");
  client1.fs->path("/foolink")->symlink("/foo");
  BOOST_CHECK_EQUAL(client1.fs->path("/foolink")->readlink(), "/foo");
  ELLE_LOG("client2 check");
  BOOST_CHECK_THROW(client2.fs->path("/foolink")->readlink(), std::exception);
  BOOST_CHECK_THROW(client2.fs->path("/foo")->open(O_RDWR, 0), std::exception);
  auto skey = serialize(client2.dht.dht->keys().K());
  client1.fs->path("/")->setxattr("infinit.auth.setrw", skey, 0);
  client1.fs->path("/")->setxattr("infinit.auth.inherit", "true", 0);
  BOOST_CHECK_THROW(client2.fs->path("/foolink")->readlink(), std::exception);
  BOOST_CHECK_THROW(client2.fs->path("/foo")->open(O_RDWR, 0), std::exception);
  client1.fs->path("/foolink2")->symlink("/foo");
  BOOST_CHECK_NO_THROW(client2.fs->path("/foolink2")->readlink());
  client1.fs->path("/foolink")->setxattr("infinit.auth.setr", skey, 0);
  BOOST_CHECK_NO_THROW(client2.fs->path("/foolink")->readlink());
}

ELLE_TEST_SCHEDULED(short_hash_key)
{
  DHTs servers(1);
  auto client1 = servers.client();
  auto key = infinit::cryptography::rsa::keypair::generate(512);
  auto serkey = elle::serialization::json::serialize(key.K());
  client1.fs->path("/")->setxattr("infinit.auth.setr", serkey.string(), 0);
  auto jsperms = client1.fs->path("/")->getxattr("infinit.auth");
  std::stringstream s(jsperms);
  auto jperms = elle::json::read(s);
  auto a = boost::any_cast<elle::json::Array>(jperms);
  BOOST_CHECK_EQUAL(a.size(), 2);
  auto hash = boost::any_cast<std::string>(
    boost::any_cast<elle::json::Object>(a.at(1)).at("name"));
  ELLE_TRACE("got hash: %s", hash);
  client1.fs->path("/")->setxattr("infinit.auth.clear", hash, 0);
  jsperms = client1.fs->path("/")->getxattr("infinit.auth");
  s.str(jsperms);
  jperms = elle::json::read(s);
  a = boost::any_cast<elle::json::Array>(jperms);
  BOOST_CHECK_EQUAL(a.size(), 1);
  BOOST_CHECK_THROW(client1.fs->path("/")->setxattr("infinit.auth.clear", "#gogol", 0),
                    std::exception);
}

ELLE_TEST_SCHEDULED(rename_exceptions)
{
  // Ensure source does not get erased if rename fails under various conditions
  DHTs servers(-1);
  auto client1 = servers.client();
  client1.fs->path("/");
  auto client2 = servers.client(true);
  BOOST_CHECK_THROW(client2.fs->path("/foo")->mkdir(0666), std::exception);
  auto c2key = elle::serialization::json::serialize(client2.dht.dht->keys().K()).string();
  client1.fs->path("/")->setxattr("infinit.auth.setrw", c2key, 0);
  ELLE_TRACE("create target inaccessible dir");
  client1.fs->path("/dir")->mkdir(0600);
  ELLE_TRACE("mkdir without perms");
  BOOST_CHECK_THROW(client2.fs->path("/dir/foo")->mkdir(0666), std::exception);
  ELLE_TRACE("create source dir");
  client2.fs->path("/foo")->mkdir(0666);
  ELLE_TRACE("Rename");
  try
  {
    client2.fs->path("/foo")->rename("/dir/foo");
    BOOST_CHECK(false);
  }
  catch (elle::Error const&e)
  {
    ELLE_TRACE("exc %s", e);
  }
  struct stat st;
  client2.fs->path("/foo")->stat(&st);
  BOOST_CHECK(S_ISDIR(st.st_mode));
  // check again with read-only access
  client1.fs->path("/dir")->setxattr("infinit.auth.setr", c2key, 0);
  ELLE_TRACE("Rename2");
  try
  {
    client2.fs->path("/foo")->rename("/dir/foo");
    BOOST_CHECK(false);
  }
  catch (elle::Error const&e)
  {
    ELLE_TRACE("exc %s", e);
  }
  client2.fs->path("/foo")->stat(&st);
  BOOST_CHECK(S_ISDIR(st.st_mode));
}


ELLE_TEST_SCHEDULED(erased_group)
{
  DHTs servers(-1);
  auto client1 = servers.client();
  auto client2 = servers.client(true);
  auto c2key = elle::serialization::json::serialize(client2.dht.dht->keys().K()).string();
  client1.fs->path("/");
  client1.fs->path("/")->setxattr("infinit.group.create", "grp", 0);
  client1.fs->path("/")->setxattr("infinit.group.add", "grp:" + c2key, 0);
  client1.fs->path("/")->setxattr("infinit.auth.setrw", "@grp", 0);
  client1.fs->path("/")->setxattr("infinit.auth.inherit", "true", 0);
  client2.fs->path("/dir")->mkdir(0666);
  client2.fs->path("/file")->create(O_RDWR | O_CREAT, 0666)->write(
    elle::ConstWeakBuffer("foo", 3), 3, 0);
  client1.fs->path("/")->setxattr("infinit.group.delete", "grp", 0);
  // cant write to /, because last author is a group member: it fails validation
  BOOST_CHECK_THROW(client1.fs->path("/dir2")->mkdir(0666), reactor::filesystem::Error);
  // we have inherit enabled, copy_permissions will fail on the missing group
  BOOST_CHECK_THROW(client2.fs->path("/dir/dir")->mkdir(0666), reactor::filesystem::Error);
  client2.fs->path("/file")->open(O_RDWR, 0644)->write(
    elle::ConstWeakBuffer("bar", 3), 3, 0);
}

ELLE_TEST_SCHEDULED(erased_group_recovery)
{
  DHTs servers(-1);
  auto client1 = servers.client();
  auto client2 = servers.client(true);
  client1.fs->path("/");
  auto c2key = elle::serialization::json::serialize(client2.dht.dht->keys().K()).string();
  client1.fs->path("/")->setxattr("infinit.group.create", "grp", 0);
  client1.fs->path("/")->setxattr("infinit.group.add", "grp:" + c2key, 0);
  ELLE_TRACE("set group ACL");
  client1.fs->path("/")->setxattr("infinit.auth.setrw", "@grp", 0);
  client1.fs->path("/")->setxattr("infinit.auth.inherit", "true", 0);
  client1.fs->path("/dir")->mkdir(0666);
  ELLE_TRACE("delete group");
  client1.fs->path("/")->setxattr("infinit.group.delete", "grp", 0);
  ELLE_TRACE("list auth");
  auto jsperms = client1.fs->path("/dir")->getxattr("infinit.auth");
  std::stringstream s(jsperms);
  auto jperms = elle::json::read(s);
  auto a = boost::any_cast<elle::json::Array>(jperms);
  BOOST_CHECK_EQUAL(a.size(), 2);
  auto hash = boost::any_cast<std::string>(
    boost::any_cast<elle::json::Object>(a.at(1)).at("name"));
  ELLE_TRACE("got hash: %s", hash);
  ELLE_TRACE("clear group from auth");
  client1.fs->path("/dir")->setxattr("infinit.auth.clear", hash, 0);
  ELLE_TRACE("recheck auth");
  jsperms = client1.fs->path("/dir")->getxattr("infinit.auth");
  s.str(jsperms);
  jperms = elle::json::read(s);
  a = boost::any_cast<elle::json::Array>(jperms);
  BOOST_CHECK_EQUAL(a.size(), 1);
  client1.fs->path("/")->setxattr("infinit.auth.clear", hash, 0);
  jsperms = client1.fs->path("/")->getxattr("infinit.auth");
  s.str(jsperms);
  jperms = elle::json::read(s);
  a = boost::any_cast<elle::json::Array>(jperms);
  BOOST_CHECK_EQUAL(a.size(), 1);
}

ELLE_TEST_SCHEDULED(remove_permissions)
{
  DHTs servers(-1);
  auto client1 = servers.client(false);
  auto client2 = servers.client(true);
  auto skey = serialize(client2.dht.dht->keys().K());
  client1.fs->path("/dir")->mkdir(0666);
  client1.fs->path("/")->setxattr("infinit.auth.setr", skey, 0);
  client1.fs->path("/dir")->setxattr("infinit.auth.setrw", skey, 0);
  client1.fs->path("/dir")->setxattr("infinit.auth.inherit", "true", 0);
  auto h = client2.fs->path("/dir/file")->create(O_CREAT|O_TRUNC|O_RDWR, 0666);
  h->write(elle::ConstWeakBuffer("foo", 3), 3, 0);
  h->close();
  h.reset();
  h = client1.fs->path("/dir/file")->open(O_RDONLY, 0666);
  char buf[512] = {0};
  int len = h->read(elle::WeakBuffer(buf, 512), 512, 0);
  BOOST_CHECK_EQUAL(len, 3);
  BOOST_CHECK_EQUAL(buf, std::string("foo"));
  h->close();
  h.reset();
  client2.fs->path("/dir/file")->unlink();
  struct stat st;
  client2.fs->path("/dir")->stat(&st);
  BOOST_CHECK(st.st_mode & S_IFDIR);
  int count = 0;
  client1.fs->path("/dir")->list_directory(
      [&](std::string const&, struct stat*) { ++count;});
  BOOST_CHECK_EQUAL(count, 2);

  h = client1.fs->path("/file")->create(O_CREAT|O_TRUNC|O_RDWR, 0666);
  h->write(elle::ConstWeakBuffer("bar", 3), 3, 0);
  h->close();
  h.reset();
  client1.fs->path("/file")->setxattr("infinit.auth.setr", skey, 0);
  BOOST_CHECK_THROW(client2.fs->path("/file")->unlink(), std::exception);
  client1.fs->path("/file")->setxattr("infinit.auth.setrw", skey, 0);
  BOOST_CHECK_THROW(client2.fs->path("/file")->unlink(), std::exception);
  client1.fs->path("/file")->setxattr("infinit.auth.setr", skey, 0);
  client1.fs->path("/")->setxattr("infinit.auth.setrw", skey, 0);
  BOOST_CHECK_THROW(client2.fs->path("/file")->unlink(), std::exception);
  h = client1.fs->path("/file")->open(O_RDONLY, 0666);
  len = h->read(elle::WeakBuffer(buf, 512), 512, 0);
  BOOST_CHECK_EQUAL(len, 3);
  BOOST_CHECK_EQUAL(buf, std::string("bar"));

  client1.fs->path("/dir2")->mkdir(0666);
  BOOST_CHECK_THROW(client2.fs->path("/dir2")->rmdir(), std::exception);
  client1.fs->path("/")->setxattr("infinit.auth.setr", skey, 0);
  client1.fs->path("/dir2")->setxattr("infinit.auth.setrw", skey, 0);
  BOOST_CHECK_THROW(client2.fs->path("/dir2")->rmdir(), std::exception);
  client1.fs->path("/")->setxattr("infinit.auth.setrw", skey, 0);
  BOOST_CHECK_NO_THROW(client2.fs->path("/dir2")->rmdir());
}

ELLE_TEST_SCHEDULED(create_excl)
{
  DHTs servers(1, with_cache = true);
  auto client1 = servers.client(false);
  auto client2 = servers.client(false);
  // cache feed
  client1.fs->path("/");
  client2.fs->path("/");
  client1.fs->path("/file")->create(O_RDWR|O_CREAT|O_EXCL, 0644);
  BOOST_CHECK_THROW(
    client2.fs->path("/file")->create(O_RDWR|O_CREAT|O_EXCL, 0644),
    reactor::filesystem::Error);
  // again, now that our cache knows the file
  BOOST_CHECK_THROW(
    client2.fs->path("/file")->create(O_RDWR|O_CREAT|O_EXCL, 0644),
    reactor::filesystem::Error);
}

ELLE_TEST_SCHEDULED(sparse_file)
{
  // Under windows, a 'cp' causes a ftruncate(target_size), so check that it
  // works
  DHTs servers(-1);
  auto client = servers.client();
  client.fs->path("/");
  for (int iter = 0; iter < 2; ++iter)
  { // run twice to get 'non-existing' and 'existing' initial states
    auto h = client.fs->path("/file")->create(O_RDWR | O_CREAT|O_TRUNC, 0666);
    char buf[191];
    char obuf[191];
    for (int i=0; i<191; ++i)
      buf[i] = i%191;
    int sz = 191 * (1 + 2500000/191);
    h->ftruncate(sz);
    for (int i=0;i<2500000; i+= 191)
    {
      h->write(elle::ConstWeakBuffer(buf, 191), 191, i);
    }
    h->close();
    h = client.fs->path("/file")->open(O_RDONLY, 0666);
    for (int i=0;i<2500000; i+= 191)
    {
      h->read(elle::WeakBuffer(obuf, 191), 191, i);
      BOOST_CHECK(!memcmp(obuf, buf, 191));
    }
  }
}

ELLE_TEST_SUITE()
{
  // This is needed to ignore child process exiting with nonzero
  // There is unfortunately no more specific way.
  elle::os::setenv("BOOST_TEST_CATCH_SYSTEM_ERRORS", "no", 1);
#ifndef INFINIT_WINDOWS
  signal(SIGCHLD, SIG_IGN);
#endif
  auto& suite = boost::unit_test::framework::master_test_suite();
  // only doughnut supported filesystem->add(BOOST_TEST_CASE(test_basic), 0, 50);
  suite.add(BOOST_TEST_CASE(filesystem), 0, 120);
  suite.add(BOOST_TEST_CASE(filesystem_paxos), 0, 240);
#ifndef INFINIT_MACOSX
  // osxfuse fails to handle two mounts at the same time, the second fails
  // with a mysterious 'permission denied'
  suite.add(BOOST_TEST_CASE(acl), 0, 120);
  suite.add(BOOST_TEST_CASE(acl_paxos), 0, 240);
  suite.add(BOOST_TEST_CASE(conflicts), 0, 120);
  suite.add(BOOST_TEST_CASE(conflicts_paxos), 0, 120);
#endif
  suite.add(BOOST_TEST_CASE(write_unlink), 0, 1);
  suite.add(BOOST_TEST_CASE(write_truncate), 0, 1);
  suite.add(BOOST_TEST_CASE(prefetcher_failure), 0, 5);
  suite.add(BOOST_TEST_CASE(paxos_race), 0, 5);
  suite.add(BOOST_TEST_CASE(data_embed), 0, 5);
  suite.add(BOOST_TEST_CASE(symlink_perms), 0, 5);
  suite.add(BOOST_TEST_CASE(short_hash_key), 0, 5);
  suite.add(BOOST_TEST_CASE(rename_exceptions), 0, 5);
  suite.add(BOOST_TEST_CASE(erased_group), 0, 5);
  suite.add(BOOST_TEST_CASE(erased_group_recovery), 0, 5);
  suite.add(BOOST_TEST_CASE(remove_permissions),0, 5);
  suite.add(BOOST_TEST_CASE(create_excl),0, 5);
  suite.add(BOOST_TEST_CASE(sparse_file),0, 5);
}
