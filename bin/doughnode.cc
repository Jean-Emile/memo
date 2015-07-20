#include <boost/filesystem/fstream.hpp>
#include <boost/program_options.hpp>

#include <elle/Exit.hh>
#include <elle/cast.hh>
#include <elle/serialization/json.hh>

#include <reactor/scheduler.hh>

#include <infinit/model/Model.hh>
#include <infinit/model/doughnut/Doughnut.hh>
#include <infinit/model/doughnut/Local.hh>
#include <infinit/storage/Storage.hh>
#include <infinit/version.hh>

ELLE_LOG_COMPONENT("doughnode");

struct Config
{
public:
  boost::optional<int> port;
  std::unique_ptr<infinit::storage::StorageConfig> storage;
  std::shared_ptr<infinit::model::ModelConfig> model;

  Config()
    : port(0)
    , storage()
    , model()
  {}

  Config(elle::serialization::SerializerIn& input)
    : Config()
  {
    this->serialize(input);
  }

  void
  serialize(elle::serialization::Serializer& s)
  {
    s.serialize("port", this->port);
    s.serialize("storage", this->storage);
    s.serialize("model", this->model);
  }
};

static
void
parse_options(int argc, char** argv, Config& cfg)
{
  ELLE_TRACE_SCOPE("parse command line");
  using namespace boost::program_options;
  options_description options("Options");
  options.add_options()
    ("config,c", value<std::string>(), "configuration file")
    ("help,h", "display the help")
    ("version,v", "display version")
    ;
  variables_map vm;
  try
  {
    store(parse_command_line(argc, argv, options), vm);
    notify(vm);
  }
  catch (invalid_command_line_syntax const& e)
  {
    throw elle::Error(elle::sprintf("command line error: %s", e.what()));
  }
  if (vm.count("help"))
  {
    std::cout << "Usage: " << argv[0] << " [options]" << std::endl;
    std::cout << std::endl;
    std::cout << options;
    std::cout << std::endl;
    std::cout << "Infinit v" << INFINIT_VERSION << std::endl;
    throw elle::Exit(0);
  }
  if (vm.count("version"))
  {
    std::cout << INFINIT_VERSION << std::endl;
    throw elle::Exit(0);
  }
  if (vm.count("config") != 0)
  {
    std::string const config = vm["config"].as<std::string>();
    boost::filesystem::ifstream input_file(config);
    try
    {
      elle::serialization::json::SerializerIn input(input_file, false);
      input.serialize_forward(cfg);
    }
    catch (elle::Error const& e)
    {
      throw elle::Error(
        elle::sprintf("error in configuration file %s: %s", config, e.what()));
    }
  }
  else
    throw elle::Error("missing mandatory 'config' option");
}

int
main(int argc, char** argv)
{
  try
  {
    reactor::Scheduler sched;
    reactor::Thread main(
      sched,
      "main",
      [argc, argv]
      {
        Config cfg;
        parse_options(argc, argv, cfg);
        ELLE_ASSERT(cfg.model.get());
        auto model = cfg.model->make();
        if (cfg.storage)
        {
          auto storage = cfg.storage->make();
          infinit::model::doughnut::Local local(std::move(storage),
                                                cfg.port ? *cfg.port : 0);
          local.doughnut() =
            elle::cast<infinit::model::doughnut::Doughnut>::runtime(model);
          reactor::sleep();
        }
        else
          reactor::sleep();

      });
    sched.run();
  }
  catch (elle::Exit const& e)
  {
    return e.return_code();
  }
  catch (std::exception const& e)
  {
    elle::fprintf(std::cerr, "%s: fatal error: %s\n", argv[0], e.what());
    return 1;
  }
}
