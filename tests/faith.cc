#include <elle/log.hh>
#include <elle/test.hh>

#include <infinit/model/MissingBlock.hh>
#include <infinit/model/blocks/MutableBlock.hh>
#include <infinit/model/faith/Faith.hh>
#include <infinit/storage/Memory.hh>

ELLE_LOG_COMPONENT("infinit.model.faith.test");

static
void
faith()
{
  std::unique_ptr<infinit::storage::Storage> storage
    = elle::make_unique<infinit::storage::Memory>();
  infinit::model::faith::Faith faith(std::move(storage));

  auto block1 = faith.make_block<infinit::model::blocks::MutableBlock>();
  auto block2 = faith.make_block<infinit::model::blocks::MutableBlock>();
  BOOST_CHECK_NE(block1->address(), block2->address());
  ELLE_LOG("store blocks")
  {
    faith.store(*block1);
    BOOST_CHECK_EQUAL(*faith.fetch(block1->address()), *block1);
    faith.store(*block2);
    BOOST_CHECK_EQUAL(*faith.fetch(block2->address()), *block2);
  }
  ELLE_LOG("update block")
  {
    std::string update("twerk is the new twist");
    block2->data(elle::Buffer(update.c_str(), update.length()));
    BOOST_CHECK_NE(*faith.fetch(block2->address()), *block2);
    ELLE_LOG("STORE %x", block2->data());
    faith.store(*block2);
    ELLE_LOG("STORED %x", block2->data());
    BOOST_CHECK_EQUAL(*faith.fetch(block2->address()), *block2);
  }
  ELLE_LOG("fetch non-existent block")
  {
    auto block3 = faith.make_block<infinit::model::blocks::MutableBlock>();
    BOOST_CHECK_THROW(faith.fetch(block3->address()),
                      infinit::model::MissingBlock);
    BOOST_CHECK_THROW(faith.remove(block3->address()),
                      infinit::model::MissingBlock);
  }
  ELLE_LOG("remove block")
  {
    faith.remove(block2->address());
    BOOST_CHECK_THROW(faith.fetch(block2->address()),
                      infinit::model::MissingBlock);
    faith.fetch(block1->address());
  }
}


ELLE_TEST_SUITE()
{
  auto& suite = boost::unit_test::framework::master_test_suite();
  suite.add(BOOST_TEST_CASE(faith));
}
