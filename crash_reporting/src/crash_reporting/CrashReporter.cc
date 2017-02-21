#include <crash_reporting/CrashReporter.hh>

#include <elle/format/base64.hh>
#include <elle/json/json.hh>
#include <elle/log.hh>
#include <elle/system/platform.hh>
#include <elle/system/user_paths.hh>

#include <boost/filesystem/fstream.hpp>
#include <boost/range/algorithm/count_if.hpp>

#include <reactor/http/exceptions.hh>
#include <reactor/http/Request.hh>

#ifdef INFINIT_LINUX
# include <client/linux/handler/exception_handler.h>
#elif defined(INFINIT_MACOSX)
# include <crash_reporting/gcc_fix.hh>
// FIXME: Adding `pragma GCC diagnostic ignored "-Wdeprecated"` does not work
// for removing #import warnings.
# include <client/mac/handler/exception_handler.h>
#else
# error Unsupported platform.
#endif

ELLE_LOG_COMPONENT("CrashReporter");

namespace crash_reporting
{
  namespace bfs = boost::filesystem;

  namespace
  {
#if defined INFINIT_LINUX
    bool
    dump_callback(const google_breakpad::MinidumpDescriptor& descriptor,
                  void* context,
                  bool success)
#elif defined INFINIT_MACOSX
    bool
    dump_callback(const char* dump_dir,
                  const char* minidump_id,
                  void* context,
                  bool success)
#endif
    {
      return success;
    }
  }

  CrashReporter::CrashReporter(std::string crash_url,
                               bfs::path dumps_path,
                               std::string version)
    : _crash_url(std::move(crash_url))
    , _enabled(true)
    , _exception_handler(nullptr)
    , _dumps_path(std::move(dumps_path))
    , _version(std::move(version))
  {
#if defined INFINIT_LINUX
    google_breakpad::MinidumpDescriptor descriptor(this->_dumps_path.string());
    this->_exception_handler =
      new google_breakpad::ExceptionHandler(descriptor,
                                            nullptr,
                                            dump_callback,
                                            nullptr,
                                            true,
                                            -1);
#elif defined INFINIT_MACOSX
    this->_exception_handler =
      new google_breakpad::ExceptionHandler(this->_dumps_path.string(),
                                            nullptr,
                                            dump_callback,
                                            nullptr,
                                            true,
                                            nullptr);
#endif
    ELLE_TRACE("crash reporter started");
  }

  CrashReporter::~CrashReporter()
  {
    delete this->_exception_handler;
  }

  bool
  CrashReporter::enabled() const
  {
    return this->_enabled;
  }

  namespace
  {
    bool
    _is_crash_report(bfs::path const& path)
    {
      return bfs::is_regular_file(path) && path.extension().string() == ".dmp";
    }

    void
    _remove_file(bfs::path const& path)
    {
      boost::system::error_code erc;
      bfs::remove(path, erc);
      if (erc)
        ELLE_WARN("unable to remove crash dump (%s): %s", path, erc.message());
    }
  }

  int32_t
  CrashReporter::crashes_pending_upload()
  {
    int32_t res =
      boost::count_if(bfs::directory_iterator(this->_dumps_path),
                      [](auto const& p)
                      {
                        return _is_crash_report(p.path());
                      });
    ELLE_DEBUG("%s: %s %s awaiting upload",
               *this, res, (res == 1 ? "crash" : "crashes"));
    return res;
  }

  void
  CrashReporter::_upload(bfs::path const& path) const
  {
    bfs::ifstream f;
    f.open(path, std::ios_base::in | std::ios_base::binary);
    if (!f.good())
    {
      ELLE_ERR("%s: unable to read crash dump: %s", *this, path);
      return;
    }
    ELLE_DEBUG("%s: uploading: %s", *this, path);
    reactor::http::Request r(this->_crash_url,
                             reactor::http::Method::PUT,
                             "application/json");
    elle::Buffer dump;
    elle::IOStream stream(dump.ostreambuf());
    elle::format::base64::Stream base64_stream(stream);
    auto const chunk_size = 16 * 1024;
    char chunk[chunk_size + 1];
    chunk[chunk_size] = 0;
    while (!f.eof())
    {
      f.read(chunk, chunk_size);
      base64_stream.write(chunk, chunk_size);
      base64_stream.flush();
    }
    auto content = elle::json::Object
      {
        {"dump", dump.string()},
        {"platform", elle::system::platform::os_description()},
        {"version", this->_version},
      };
    ELLE_DUMP("%s: content to upload: %s", *this, content);
    elle::json::write(r, content);
    if (r.status() == reactor::http::StatusCode::OK)
    {
      ELLE_DUMP("%s: removing uploaded crash dump: %s", *this, path);
      f.close();
      _remove_file(path);
    }
    else
    {
      ELLE_ERR("%s: unable to upload crash report (%s) to %s: %s",
               *this, path, this->_crash_url, r.status());
    }
  }

  void
  CrashReporter::upload_existing() const
  {
    if (!this->_enabled)
      return;
    for (auto const& p: bfs::directory_iterator(this->_dumps_path))
    {
      auto const& path = p.path();
      if (!_is_crash_report(path))
      {
        ELLE_DUMP("%s: file is not crash dump: %s", *this, path);
        continue;
      }
      try
      {
        _upload(path);
      }
      catch (reactor::http::RequestError const& e)
      {
        ELLE_TRACE("%s: unable to complete upload of %s: %s", *this, path, e);
      }
    }
  }
}
