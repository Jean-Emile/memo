#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>
#include <fcntl.h>

#include <boost/filesystem.hpp>

#include <elle/os/environ.hh>
#include <elle/test.hh>

#include <elle/reactor/filesystem.hh>

#include <infinit/filesystem/filesystem.hh>

#include "DHT.hh"

namespace bfs = boost::filesystem;

ELLE_LOG_COMPONENT("infinit.model.doughnut.bandwidth-test");

ELLE_TEST_SCHEDULED(bazillion_small_files)
{
  auto path = bfs::temp_directory_path() / bfs::unique_path();
  elle::os::setenv("INFINIT_HOME", path.string(), true);
  elle::SafeFinally cleanup_path([&] {
      bfs::remove_all(path);
  });
  auto const k = elle::cryptography::rsa::keypair::generate(512);
  auto server_a = DHT(owner = k);
  auto client = DHT(keys = k, storage = nullptr);
  client.overlay->connect(*server_a.overlay);
  auto fs = std::make_unique<infinit::filesystem::FileSystem>(
    "test/bandwidth", client.dht,
    infinit::filesystem::allow_root_creation = true);
  elle::reactor::filesystem::FileSystem driver(std::move(fs), true);
  auto root = driver.path("/");
  int const max = std::stoi(elle::os::getenv("ITERATIONS", "100"));
  auto& storage =
    dynamic_cast<infinit::storage::Memory&>(*server_a.dht->local()->storage());
  auto resident = boost::optional<double>{};
  for (int i = 0; i < max; ++i)
  {
    ELLE_LOG_SCOPE("%4s / %s\n", i, max);
    auto file = root->child(elle::sprintf("%04s", i));
    auto handle = file->create(O_RDWR, 0666 | S_IFREG);
    auto contents = elle::Buffer(100 * 1024);
    memset(contents.mutable_contents(), 0xfd, contents.size());
    handle->write(contents, contents.size(), 0);
    handle->close();
    root->child(elle::sprintf("%04s", i))->unlink();
    if (!resident)
      resident = storage.size();
    else
      // Check storage space stays within 5%
      BOOST_CHECK_CLOSE(double(storage.size()), *resident, 5.);
  }
}

ELLE_TEST_SUITE()
{
  auto& suite = boost::unit_test::framework::master_test_suite();
  suite.add(BOOST_TEST_CASE(bazillion_small_files), 0, valgrind(100));
}
