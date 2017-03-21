#pragma once

#include <infinit/model/blocks/Block.hh>

namespace infinit
{
  namespace model
  {
    namespace blocks
    {
      class MutableBlock
        : public Block
      {
      /*------.
      | Types |
      `------*/
      public:
        using Self = infinit::model::blocks::MutableBlock;
        using Super = infinit::model::blocks::Block;

      /*-------------.
      | Construction |
      `-------------*/
        MutableBlock(MutableBlock const& other);
      protected:
        MutableBlock(Address address);
        MutableBlock(Address address, elle::Buffer data);
        friend class infinit::model::Model;
        bool _data_changed;

      /*-------.
      | Clone  |
      `-------*/
      public:

        std::unique_ptr<Block>
        clone() const override;

      /*--------------.
      | Serialization |
      `--------------*/
      public:
        MutableBlock(elle::serialization::Serializer& input,
                     elle::Version const& version);

        void
        serialize(elle::serialization::Serializer& s,
                  elle::Version const& version) override;
      private:
        void
        _serialize(elle::serialization::Serializer& s);

      /*--------.
      | Content |
      `--------*/
      public:
        using Super::data;
        virtual
        int
        version() const /* = 0 */ { return 0; }; // FIXME
        virtual
        void
        data(elle::Buffer data);
        virtual
        void
        data(std::function<void (elle::Buffer&)> transformation);
        ELLE_ATTRIBUTE_RW(boost::optional<int>, seal_version, protected);
      };
    }
  }
}
