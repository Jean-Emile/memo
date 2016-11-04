#ifndef INFINIT_MODEL_DOUGHNUT_LOCAL_HXX
# define INFINIT_MODEL_DOUGHNUT_LOCAL_HXX

# include <reactor/for-each.hh>

namespace infinit
{
  namespace model
  {
    namespace doughnut
    {
      template <typename R, typename ... Args>
      R
      Local::broadcast(std::string const& name, Args&& ... args)
      {
        ELLE_LOG_COMPONENT("infinit.model.doughnut.Local");
        ELLE_TRACE_SCOPE("%s: broadcast %s", this, name);
        // Copy peers to hold connections refcount, as for_each_parallel
        // captures values by ref.
        auto peers = this->_peers;
        reactor::for_each_parallel(
          peers,
          [&] (std::shared_ptr<Connection> const& c)
          {
            // Arguments taken by reference as they will be passed multiple
            // times.
            RPC<R (Args const& ...)> rpc(
              name,
              c->_channels,
              this->version(),
              c->_rpcs._key);
            // Workaround GCC 4.9 ICE: argument packs don't work through
            // lambdas.
            auto const f = std::bind(
              &RPC<R (Args const& ...)>::operator (),
              &rpc, std::ref(args)...);
            try
            {
              return RPCServer::umbrella(f);
            }
            catch (UnknownRPC const& e)
            {
              // FIXME: Ignore ? Evict ? Should probably be configurable. So far
              // only Kouncil uses this, and it's definitely an ignore.
              ELLE_WARN("error contacting %s: %s", c, e);
            }
          },
          elle::sprintf("%s: broadcast RPC %s", this, name));
      }
    }
  }
}

#endif
