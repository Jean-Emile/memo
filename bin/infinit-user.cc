#include <elle/log.hh>
#include <elle/serialization/Serializer.hh>
#include <elle/serialization/json.hh>

#include <das/serializer.hh>

#include <cryptography/rsa/KeyPair.hh>
#include <cryptography/rsa/pem.hh>

ELLE_LOG_COMPONENT("infinit-user");

#include <main.hh>

using namespace boost::program_options;

infinit::Infinit ifnt;

template <typename Super>
struct UserView
  : Super
{
  template <typename ... Args>
  UserView(Args&& ... args)
    : Super(std::forward<Args>(args)...)
  {}

  void
  serialize(elle::serialization::Serializer& s)
  {
    Super::serialize(s);
    std::string id(infinit::User::uid(this->object().public_key));
    s.serialize("id", id);
  }
};


static
void
export_(variables_map const& args)
{
  auto name = get_name(args);
  auto user = ifnt.user_get(name);
  auto output = get_output(args);
  if (args.count("full") && args["full"].as<bool>())
  {
    if (!script_mode)
    {
      elle::fprintf(std::cerr, "WARNING: you are exporting the user \"%s\" "
                    "including the private key\n", name);
      elle::fprintf(std::cerr, "WARNING: anyone in possession of this "
                    "information can impersonate that user\n");
      elle::fprintf(std::cerr, "WARNING: if you mean to export your user for "
                    "someone else, remove the --full flag\n");
    }
    UserView<das::Serializer<infinit::DasUser>> view(user);
    elle::serialization::json::serialize(view, *output, false);
  }
  else
  {
    UserView<das::Serializer<infinit::DasPublicUser>> view(user);
    elle::serialization::json::serialize(view, *output, false);
  }
}

static
void
fetch(variables_map const& args)
{
  auto owner = self_user(ifnt, args);
  auto user_name = mandatory(args, "name", "user name");
  auto user =
    beyond_fetch<infinit::User>("user", user_name);
  ifnt.user_save(std::move(user));
}

void
echo_mode(bool enable)
{
#if defined(INFINIT_WINDOWS)
  HANDLE hStdin = GetStdHandle(STD_INPUT_HANDLE);
  DWORD mode;
  GetConsoleMode(hStdin, &mode);
  if (!enable)
    mode &= ~ENABLE_ECHO_INPUT;
  else
    mode |= ENABLE_ECHO_INPUT;
  SetConsoleMode(hStdin, mode );
#else
  struct termios tty;
  tcgetattr(STDIN_FILENO, &tty);
  if(!enable)
    tty.c_lflag &= ~ECHO;
  else
    tty.c_lflag |= ECHO;
  (void)tcsetattr(STDIN_FILENO, TCSANOW, &tty);
#endif
}

std::string
read_passphrase()
{
  std::string res;
  {
    elle::SafeFinally restore_echo([] { echo_mode(true); });
    echo_mode(false);
    std::cout << "Passphrase: ";
    std::cout.flush();
    std::getline(std::cin, res);
  }
  std::cout << std::endl;
  return res;
}

static
void
create(variables_map const& args)
{
  auto name = get_name(args);
  auto keys_file = optional(args, "key");
  auto keys = [&] // -> infinit::cryptography::rsa::KeyPair
    {
      if (keys_file)
      {
        auto passphrase = read_passphrase();
        return infinit::cryptography::rsa::pem::import_keypair(
          keys_file.get(), passphrase);
      }
      else
      {
        report("generating RSA keypair");
        return infinit::cryptography::rsa::keypair::generate(2048);
      }
    }();
  infinit::User user(name, keys);
  ifnt.user_save(user);
  report_action("generated", "user", name);
}

static
void
import(variables_map const& args)
{
  auto input = get_input(args);
  {
    auto user =
      elle::serialization::json::deserialize<infinit::User>(*input, false);
    ifnt.user_save(user);
    report_imported("user", user.name);
  }
}

static
void
push(variables_map const& args)
{
  auto name = get_name(args);
  auto user = ifnt.user_get(name);
  das::Serializer<infinit::DasPublicUser> view(user);
  beyond_push("user", user.name, view, user);
}

static
void
signup_(variables_map const& args)
{
  create(args);
  push(args);
}

int
main(int argc, char** argv)
{
  program = argv[0];
  Modes modes {
    {
      "export",
      "Export a user for someone else to import",
      &export_,
      {},
      {
        { "name,n", value<std::string>(),
          "user to export (defaults to system user)" },
        { "full,f", bool_switch(),
          "also export private information "
          "(do not use this unless you understand the implications)" },
        { "output,o", value<std::string>(),
          "file to write user to (defaults to stdout)" },
      },
    },
    {
      "create",
      "Create a user",
      &create,
      {},
      {
        { "name,n", value<std::string>(),
          "user name (defaults to system username)" },
        { "key,k", value<std::string>(),
          "RSA key pair in PEM format - e.g. your SSH key"
            " (generated if unspecified)" },
      },
    },
    {
      "fetch",
      "Fetch a user",
      &fetch,
      "--name USER",
      {
        { "name,n", value<std::string>(), "user to fetch" },
        option_owner,
      },
    },
    {
      "import",
      "Import a user",
      &import,
      {},
      {
        { "input,i", value<std::string>(),
          "file to read user from (defaults to stdin)" },
      },
    },
    {
      "push",
      elle::sprintf("Push a user to %s", beyond()).c_str(),
      &push,
      {},
      {
        { "name,n", value<std::string>(),
          "user to push (defaults to system user)" },
      },
    },
    {
      "register",
      elle::sprintf("Push user to %s (alias for --push)", beyond()).c_str(),
      &push,
      {},
      {
        { "name,n", value<std::string>(),
          "user to register (defaults to system user)" },
      },
    },
    {
      "signup",
      "Create and register a user",
      &signup_,
      {},
      {
        { "name,n", value<std::string>(),
          "user name (defaults to system username)" },
        { "key,k", value<std::string>(),
          "RSA key pair in PEM format - e.g. your SSH key"
            " (generated if unspecified)" },
      },
    },
  };
  return infinit::main("Infinit user utility", modes, argc, argv);
}
