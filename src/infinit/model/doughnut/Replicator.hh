#ifndef INFINIT_MODEL_DOUGHNUT_REPLICATOR_HH
# define INFINIT_MODEL_DOUGHNUT_REPLICATOR_HH

# include <boost/filesystem.hpp>

# include <reactor/thread.hh>

# include <infinit/model/doughnut/Consensus.hh>


namespace infinit
{
  namespace model
  {
    namespace doughnut
    {
      class Replicator: public Consensus
      {
      public:
        Replicator(Doughnut& doughnut, int factor,
                   boost::filesystem::path const& journal_dir);
        ~Replicator();
        ELLE_ATTRIBUTE_R(int, factor);
      protected:
        virtual
        void
        _store(overlay::Overlay& overlay, blocks::Block& block, StoreMode mode) override;
        virtual
        std::unique_ptr<blocks::Block>
        _fetch(overlay::Overlay& overlay, Address address) override;
        virtual
        void
        _remove(overlay::Overlay& overlay, Address address) override;
        void
        _process_cache();
        void
        _process_loop();
        overlay::Overlay* _overlay;
        boost::filesystem::path _journal_dir;
        reactor::Thread _process_thread;
      };
    }
  }
}

#endif