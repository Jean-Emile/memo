#include <elle/os/environ.hh>

DAS_MODEL_FIELDS(infinit::model::doughnut::Remote::Auth,
                 (id)(challenge)(passport));

namespace infinit
{
  namespace model
  {
    namespace doughnut
    {
      DAS_MODEL_DEFINE(infinit::model::doughnut::Remote::Auth,
                       (id)(challenge)(passport),
                       DasAuth);

      template<typename F, typename ...Args>
      typename RPC<F>::result_type
      remote_call_next(RemoteRPC<F>* ptr, Args const& ... args)
      {
        return ptr->RPC<F>::operator()(args...);
      }

      template<typename F>
      template<typename ...Args>
      typename RPC<F>::result_type
      RemoteRPC<F>::operator()(Args const& ... args)
      {
        // GCC bug, argument packs dont work in lambdas
        auto helper = std::bind(&remote_call_next<F, Args...>,
          this, std::ref(args)...);
        return _remote->safe_perform<typename RPC<F>::result_type>("RPC",
          [&] {
            this->_channels = _remote->channels().get();
            auto creds = _remote->credentials();
            if (!creds.empty())
            {
              elle::Buffer c(creds);
              this->key().emplace(std::move(c));
            }
            return helper();
        });
      }

      template<typename R>
      R
      Remote::safe_perform(std::string const& name,
                           std::function<R()> op)
      {
        ELLE_LOG_COMPONENT("infinit.model.doughnut.Remote");
        // We use one timeout for each connect attempt (in order to maybe
        // refresh endpoints), and a second timeout for the overall operation
        // setting INFINIT_SOFTFAIL_TIMEOUT to 0 will retry forever
        static const int connect_timeout_sec =
          std::stoi(elle::os::getenv("INFINIT_CONNECT_TIMEOUT", "10"));
        static const int softfail_timeout_sec =
          std::stoi(elle::os::getenv("INFINIT_SOFTFAIL_TIMEOUT", "50"));
        static const bool enable_fast_fail =
          !elle::os::getenv("INFINIT_SOFTFAIL_FAST", "true").empty();
        elle::DurationOpt connect_timeout;
        if (connect_timeout_sec)
          connect_timeout = boost::posix_time::seconds(connect_timeout_sec);
        int max_attempts = 0;
        if (softfail_timeout_sec && connect_timeout_sec)
        {
          int sts = std::max(softfail_timeout_sec, connect_timeout_sec);
          max_attempts = sts / connect_timeout_sec;
        }
        int attempt = 0;
        bool need_reconnect = false;
        /* Fast-fail mode: The idea is that the first failed operation
        * will retry for the full INFINIT_SOFTFAIL_TIMEOUT, but subsequent
        * operations will fail 'instantly', while trying to reconnect in
        * the background.
        */
        if (_fast_fail && enable_fast_fail)
        {
          bool connect_running = false;
          try
          {
            if (!this->_connected)
            { // still connecting
              ELLE_DEBUG("still connecting");
              connect_running = true;
              throw reactor::network::ConnectionClosed("Connection pending");
            }
            // if we reach here, connection thread finished without exception,
            // go on
            _fast_fail = false;
          }
          catch (reactor::network::Exception const& e)
          {
            if (connect_running)
              throw;
            ELLE_DEBUG("connection attempt failed, restarting");
            try
            {
              reconnect(0_sec);
            }
            catch (reactor::network::Exception const& e)
            {}
            throw reactor::network::ConnectionClosed("Connection attempt restarted");
          }
        }
        while (true)
        {
          // We need to know if someone else made a reconnection attempt
          // while we were waiting, and if so try this new connection
          // without making a reconnect of our own
          int prev_reconnection_id = _reconnection_id;
          try
          {
            if (need_reconnect)
              reconnect(connect_timeout);
            else
              connect(connect_timeout);
            return op();
          }
          catch(reactor::network::Exception const& e)
          {
            ELLE_TRACE("network exception when invoking %s (attempt %s/%s): %s",
                       name, attempt+1, max_attempts, e);
          }
          catch(infinit::protocol::Serializer::EOF const& e)
          {
            ELLE_TRACE("EOF when invoking %s (attempt %s/%s): %s",
                       name, attempt+1, max_attempts, e);
          }
          if (max_attempts && ++attempt >= max_attempts)
          {
            _fast_fail = true;
            throw reactor::network::ConnectionClosed(elle::sprintf("could not establish channel for operation '%s'",
                                            name));
          }
          reactor::sleep(boost::posix_time::milliseconds(
            200 * std::min(10, attempt)));
          need_reconnect = (_reconnection_id == prev_reconnection_id);
        }
      }

      template <typename F>
      RemoteRPC<F>::RemoteRPC(std::string name, Remote* remote)
        : Super(name,
                *remote->channels(),
                remote->doughnut().version(),
                elle::unconst(&remote->credentials()))
        , _remote(remote)
      {
        this->set_context(remote);
      }

      template<typename F>
      RemoteRPC<F>
      Remote::make_rpc(std::string const& name)
      {
        return RemoteRPC<F>(name, this);
      }
    }
  }
}

DAS_MODEL_DEFAULT(infinit::model::doughnut::Remote::Auth,
                  infinit::model::doughnut::DasAuth);
DAS_MODEL_SERIALIZE(infinit::model::doughnut::Remote::Auth);
