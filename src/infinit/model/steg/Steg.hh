#pragma once

#include <boost/random.hpp>
#include <boost/filesystem.hpp>
#include <infinit/model/Model.hh>
#include <infinit/model/blocks/Block.hh>

namespace infinit
{
  namespace model
  {
    namespace steg
    {
      class Steg: public Model
      {
      public:
        Steg(boost::filesystem::path const& storage, std::string const& pass);
      protected:
        std::unique_ptr<blocks::MutableBlock>
        _make_mutable_block() const override;
        void
        _store(blocks::Block& block) override;
        std::unique_ptr<blocks::Block>
        _fetch(Address address) const override;
        void
        _remove(Address address) override;
      private:
        std::unique_ptr<blocks::Block>
        __fetch(Address address) const;
        void
        __store(blocks::Block& block);
        // Pick from free blocks
        Address _pick() const;
        mutable boost::filesystem::path _storage_path;
        mutable std::string _passphrase;
        mutable std::vector<boost::filesystem::path> _free_blocks;
        mutable std::vector<Address> _used_blocks;
        mutable std::unordered_map<Address, boost::filesystem::path> _cache;
        mutable boost::random::mt19937 _rng;
        mutable std::unique_ptr<blocks::Block> _root_data;
        mutable boost::optional<Address> _root;
      };
    }
  }
}
