#include <elle/test.hh>

#include <infinit/storage/Collision.hh>
#include <infinit/storage/Memory.hh>
#include <infinit/storage/MissingKey.hh>
#include <infinit/storage/Storage.hh>

static
void
memory()
{
  infinit::storage::Memory storage;
  infinit::storage::Key::Value v1 = {
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1
  };
  infinit::storage::Key k1(&v1[0]);
  char const* data1 = "the grey";
  storage.set(k1, elle::Buffer(data1, strlen(data1)));
  BOOST_CHECK_EQUAL(storage.get(k1), data1);
  char const* data2 = "the white";
  storage.set(k1, elle::Buffer(data2, strlen(data2)), false, true);
  BOOST_CHECK_EQUAL(storage.get(k1), data2);
  BOOST_CHECK_THROW(storage.set(k1, elle::Buffer()),
                    infinit::storage::Collision);
  BOOST_CHECK_EQUAL(storage.get(k1), data2);
  infinit::storage::Key::Value v2 = {
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 2
  };
  infinit::storage::Key k2(&v2[0]);
  BOOST_CHECK_THROW(storage.get(k2), infinit::storage::MissingKey);
  BOOST_CHECK_THROW(storage.set(k2, elle::Buffer(), false, true),
                    infinit::storage::MissingKey);
}

ELLE_TEST_SUITE()
{
  auto& suite = boost::unit_test::framework::master_test_suite();
  suite.add(BOOST_TEST_CASE(memory));
}
