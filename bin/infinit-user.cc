#include <elle/log.hh>

#include <reactor/http/url.hh>

#include <das/serializer.hh>

#include <cryptography/rsa/KeyPair.hh>
#include <cryptography/rsa/pem.hh>

ELLE_LOG_COMPONENT("infinit-user");

#include <main.hh>

#include <email.hh>
#include <password.hh>

infinit::Infinit ifnt;

using boost::program_options::variables_map;

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

inline
std::string
get_name(variables_map const& args, std::string const& name = "name")
{
  return get_username(args, name);
}

void
upload_avatar(infinit::User& self,
              boost::filesystem::path const& avatar_path);
void
fetch_avatar(std::string const& name);
void
pull_avatar(infinit::User& self);
boost::optional<boost::filesystem::path>
avatar_path(std::string const& name);

COMMAND(export_)
{
  auto name = get_name(args);
  auto user = ifnt.user_get(name);
  auto output = get_output(args);
  auto avatar = avatar_path(name);
  if (avatar)
    user.avatar_path = avatar.get().string();
  if (flag(args, "full"))
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
  report_exported(*output, "user", user.name);
}

COMMAND(fetch)
{
  auto user_names =
    mandatory<std::vector<std::string>>(args, "name", "user name");
  for (auto const& name: user_names)
  {
    auto avatar = [&] () {
      if (!flag(args, "no-avatar"))
      {
        try
        {
          fetch_avatar(name);
        }
        catch (elle::Error const&)
        {}
      }
    };
    try
    {
      auto user = beyond_fetch<infinit::User>("user",
                                              reactor::http::url_encode(name));
      ifnt.user_save(std::move(user));
      avatar();
    }
    catch (ResourceAlreadyFetched const& e)
    {
      avatar();
      throw;
    }
  }
}

std::string
hub_password_hash(variables_map const& args)
{
  return hash_password(_password(args, "password", "Password"),
                       _hub_salt);
}

static
void
_push(variables_map const& args, infinit::User& user, bool atomic)
{
  auto email = optional(args, "email");
  if (email && !valid_email(email.get()))
    throw CommandLineError("invalid email address");
  bool user_updated = false;
  if (!user.email && !email)
  {
    throw CommandLineError(elle::sprintf(
      "users pushed to %s must have an email address (use --email)",
      beyond(true)));
  }
  if (email) // Overwrite existing email.
  {
    user.email = email;
    user_updated = true;
  }
  auto avatar_path = optional(args, "avatar");
  if (avatar_path && avatar_path.get().length() > 0)
  {
    if (!boost::filesystem::exists(avatar_path.get()))
      throw CommandLineError(
        elle::sprintf("%s doesn't exist", avatar_path.get()));
  }
  auto fullname = optional(args, "fullname");
  if (fullname) // Overwrite existing fullname.
  {
    user.fullname = fullname;
    user_updated = true;
  }
  auto ldap_dn = optional(args, "ldap-name");
  if (ldap_dn)
    user.ldap_dn = *ldap_dn;
  if (flag(args, "full"))
  {
    if (!ldap_dn)
      user.password_hash = hub_password_hash(args);
    das::Serializer<infinit::DasPrivateUserPublish> view{user};
    beyond_push("user", user.name, view, user);
  }
  else
  {
    if (args.count("password"))
    {
      throw CommandLineError(
        "Password is only used when pushing a full user");
    }
    das::Serializer<infinit::DasPublicUserPublish> view{user};
    beyond_push("user", user.name, view, user);
  }
  if (user_updated && !atomic)
    ifnt.user_save(user, true);
  if (avatar_path)
  {
    if (avatar_path.get().length() > 0)
      upload_avatar(user, avatar_path.get());
    else
      pull_avatar(user);
  }
}

static
infinit::User
create_(std::string const& name,
        boost::optional<std::string> keys_file,
        boost::optional<std::string> email,
        boost::optional<std::string> fullname,
        boost::optional<std::string> ldap_name,
        boost::optional<std::string> description)
{
  auto keys = [&] // -> infinit::cryptography::rsa::KeyPair
  {
    if (keys_file)
    {
      auto passphrase = read_passphrase("Key passphrase");
      return infinit::cryptography::rsa::pem::import_keypair(
          keys_file.get(), passphrase);
    }
    else
    {
      report("generating RSA keypair");
      return infinit::cryptography::rsa::keypair::generate(2048);
    }
  }();

  return infinit::User{name, keys, email, fullname, ldap_name, description};
}

COMMAND(create)
{
  bool push = aliased_flag(args, {"push-user", "push"});
  auto has_output = optional(args, "output");
  auto output = has_output ? get_output(args) : nullptr;
  auto ldap_name = optional(args, "ldap-name");
  auto full = flag(args, "full");
  if (!push)
  {
    if (ldap_name)
      throw CommandLineError("LDAP can only be used with the Hub, add --push");
    if (full || flag(args, "password"))
    {
      throw CommandLineError(
        elle::sprintf("--full and --password are only used when pushing "
                      "a user to %s", beyond(true)));
    }
  }
  if (ldap_name && !full)
    throw CommandLineError("LDAP user creation requires --full");
  auto name = get_name(args);
  auto email = optional(args, "email");
  if (email && !valid_email(email.get()))
    throw CommandLineError("invalid email address");
  infinit::User user = create_(name,
                               optional(args, "key"),
                               email,
                               optional(args, "fullname"),
                               ldap_name,
                               optional(args, "description"));
  if (output)
  {
    save(*output, user);
    report_exported(*output, "user", user.name);
  }
  else
  {
    ifnt.user_save(user);
    report_action("generated", "user", name, std::string("locally"));
  }
  if (push)
    _push(args, user, false);
}

COMMAND(import)
{
  auto input = get_input(args);
  {
    auto user =
      elle::serialization::json::deserialize<infinit::User>(*input, false);
    ifnt.user_save(user);
    report_imported("user", user.name);
  }
}

COMMAND(push)
{
  auto name = get_name(args);
  auto user = ifnt.user_get(name);
  _push(args, user, false);
}

COMMAND(pull)
{
  auto self = self_user(ifnt, args);
  auto user = get_name(args);
  beyond_delete("user", user, self, false, flag(args, "purge"));
}

COMMAND(delete_)
{
  auto name = get_name(args);
  auto user = ifnt.user_get(name);
  bool purge = flag(args, "purge");
  if (user.private_key && (!flag(args, "force") || script_mode))
  {
    std::string res;
    {
      std::cout
        << "WARNING: The local copy of the user's private key will be removed."
        << std::endl
        << "WARNING: You will no longer be able to perform actions on "
        << beyond(true) << std::endl
        << "WARNING: for this user." << std::endl
        << std::endl
        << "Confirm the name of the user you would like to delete: ";
      std::getline(std::cin, res);
    }
    if (res != user.name)
      throw elle::Error("Aborting...");
  }
  if (flag(args, "pull"))
  {
    try
    {
      auto self = self_user(ifnt, args);
      beyond_delete("user", name, self, true, purge);
    }
    catch (MissingLocalResource const& e)
    {
      throw elle::Error("unable to pull user, ensure the user has been set "
                        "using --as or INFINIT_USER");
    }
  }
  if (purge)
  {
    // XXX Remove volumes and drives that are on network owned by this user.
    // Currently only the owner of a network can create volumes/drives.
    auto owner = [] (std::string const& qualified_name) {
      return qualified_name.substr(0, qualified_name.find("/"));
    };
    for (auto const& drive_: ifnt.drives_get())
    {
      auto drive = drive_.name;
      if (owner(drive) != user.name)
        continue;
      auto drive_path = ifnt._drive_path(drive);
      if (boost::filesystem::remove(drive_path))
        report_action("deleted", "drive", drive, std::string("locally"));
    }
    for (auto const& volume_: ifnt.volumes_get())
    {
      auto volume = volume_.name;
      if (owner(volume) != user.name)
        continue;
      auto volume_path = ifnt._volume_path(volume);
      if (boost::filesystem::remove(volume_path))
        report_action("deleted", "volume", volume, std::string("locally"));
    }
    for (auto const& pair: ifnt.passports_get())
    {
      auto network = pair.first.network();
      if (owner(network) != user.name &&
          pair.second != user.name)
      {
        continue;
      }
      auto passport_path = ifnt._passport_path(network, pair.second);
      if (boost::filesystem::remove(passport_path))
      {
        report_action("deleted", "passport",
                      elle::sprintf("%s: %s", network, pair.second),
                      std::string("locally"));
      }
    }
    for (auto const& network_: ifnt.networks_get(user))
    {
      auto network = network_.name;
      if (owner(network) == user.name)
        ifnt.network_delete(network, user, true, true);
      else
        ifnt.network_unlink(network, user, true);
    }
  }
  if (avatar_path(name))
    boost::filesystem::remove(avatar_path(name).get());
  auto path = ifnt._user_path(user.name);
  if (boost::filesystem::remove(path))
    report_action("deleted", "user", user.name, std::string("locally"));
  else
  {
    throw elle::Error(
      elle::sprintf("File for user could not be deleted: %s", path));
  }
}

COMMAND(signup_)
{
  auto name = get_name(args);
  auto email = mandatory(args, "email");
  if (!valid_email(email))
    throw CommandLineError("invalid email address");
  infinit::User user = create_(name,
                               optional(args, "key"),
                               email,
                               optional(args, "fullname"),
                               optional(args, "ldap-name"),
                               optional(args, "description"));
  try
  {
    ifnt.user_get(name);
  }
  catch (elle::Error const&)
  {
    _push(args, user, true);
    ifnt.user_save(user, true);
    return;
  }
  throw elle::Error(elle::sprintf("User %s already exists locally", name));
}

COMMAND(login)
{
  auto name = get_name(args);
  auto pass = _password(args, "password", "Password");
  auto hashed_pass = hash_password(pass, _hub_salt);
  LoginCredentials c{ name, hashed_pass, pass };
  das::Serializer<DasLoginCredentials> credentials{c};
  auto json = beyond_login(name, credentials);
  elle::serialization::json::SerializerIn input(json, false);
  auto user = input.deserialize<infinit::User>();
  ifnt.user_save(user, true);
  report_action("saved", "user", name, std::string("locally"));
}

COMMAND(list)
{
  auto users = ifnt.users_get();
  std::sort(users.begin(), users.end(),
            [] (infinit::User const& lhs, infinit::User const& rhs)
            { return lhs.name < rhs.name; });
  if (script_mode)
  {
    elle::json::Array l;
    for (auto const& user: users)
    {
      elle::json::Object o;
      o["name"] = static_cast<std::string>(user.name);
      o["has_private_key"] = bool(user.private_key);
      if (user.description)
        o["description"] = user.description.get();
      l.push_back(std::move(o));
    }
    elle::json::write(std::cout, l);
  }
  else
    for (auto const& user: users)
    {
      auto self = self_user_name(args);
      if (user.name == self)
        std::cout << "* ";
      else
        std::cout << "  ";
      std::cout << user.name;
      if (user.description)
        std::cout << " \"" << user.description.get() << "\"";
      std::cout << ": public";
      if (user.private_key)
        std::cout << "/private keys";
      else
        std::cout << " key only";
      std::cout << std::endl;
    }
}

COMMAND(hash_)
{
  auto name = get_name(args);
  auto user = ifnt.user_get(name);
  auto key_hash = infinit::model::doughnut::short_key_hash(user.public_key);
  if (script_mode)
  {
    elle::json::Object res;
    res[name] = key_hash;
    elle::json::write(std::cout, res);
  }
  else
    std::cout << key_hash << std::endl;
}

template <typename Buffer>
void
_save_avatar(std::string const& name,
             Buffer const& buffer)
{
  boost::filesystem::ofstream f;
  ifnt._open_write(f, ifnt._user_avatar_path(name),
                   name, "avatar", true, std::ios::out | std::ios::binary);
  f.write(reinterpret_cast<char const*>(buffer.contents()), buffer.size());
  report_action("saved", "avatar", name, std::string("locally"));
}

void
upload_avatar(infinit::User& self,
              boost::filesystem::path const& avatar_path)
{
  boost::filesystem::ifstream icon;
  ifnt._open_read(icon, avatar_path, self.name, "icon");
  std::string s(
    std::istreambuf_iterator<char>{icon},
    std::istreambuf_iterator<char>{});
  elle::ConstWeakBuffer data(s.data(), s.size());
  auto url = elle::sprintf("users/%s/avatar", self.name);
  beyond_push_data(url, "avatar", self.name, data, "image/jpeg", self);
  _save_avatar(self.name, data);
}

void
fetch_avatar(std::string const& name)
{
  auto url = elle::sprintf("users/%s/avatar", name);
  auto request = beyond_fetch_data(url, "avatar", name);
  if (request->status() == reactor::http::StatusCode::OK)
  {
    auto response = request->response();
    // XXX: Deserialize XML.
    if (response.size() == 0 || response[0] == '<')
      throw MissingResource(
        elle::sprintf("avatar for %s not found on %s", name, beyond(true)));
    _save_avatar(name, response);
  }
}

void
pull_avatar(infinit::User& self)
{
  auto url = elle::sprintf("users/%s/avatar", self.name);
  beyond_delete(url, "avatar", self.name, self);
}

boost::optional<boost::filesystem::path>
avatar_path(std::string const& name)
{
  auto path = ifnt._user_avatar_path(name);
  if (!boost::filesystem::exists(path))
    return boost::optional<boost::filesystem::path>{};
  return path;
}

int
main(int argc, char** argv)
{
  using boost::program_options::value;
  using boost::program_options::bool_switch;
  Mode::OptionDescription option_push_full =
    { "full", bool_switch(), "include private key in order "
      "to facilitate device pairing and fetching lost keys" };
  Mode::OptionDescription option_push_password =
    { "password", value<std::string>(), elle::sprintf(
      "password to authenticate with %s. Used with --full "
      "(default: prompt for password)", beyond(true)) };
  Mode::OptionDescription option_fullname =
    { "fullname", value<std::string>(), "fullname of user (optional)" };
  Mode::OptionDescription option_avatar =
    { "avatar", value<std::string>(), "path to an image to use as avatar" };
  Mode::OptionDescription option_key =
    { "key,k", value<std::string>(),
      "RSA key pair in PEM format - e.g. your SSH key "
      "(default: generate key pair)" };
  Mode::OptionDescription option_ldap_dn =
    { "ldap-name,l", value<std::string>(),
      "LDAP distinguished name of the user, "
      "enables authentication through LDAP" };
  Modes modes {
    {
      "create",
      "Create a user",
      &create,
      {},
      {
        { "name,n", value<std::string>(), "user name (default: system user)" },
        option_description("user"),
        option_key,
        { "push-user", bool_switch(),
          elle::sprintf("push the user to %s", beyond(true)) },
        { "push,p", bool_switch(), "alias for --push-user" },
        { "email", value<std::string>(),
          "valid email address\n(mandatory when using --push-user)" },
        option_fullname,
        option_push_full,
        option_push_password,
        option_ldap_dn,
        option_output("user"),
      },
    },
    {
      "export",
      "Export a user so that it may be imported elsewhere",
      &export_,
      {},
      {
        { "name,n", value<std::string>(),
          "user to export (default: system user)" },
        { "full", bool_switch(), "include private key "
          "(do not use this unless you understand the implications)" },
        option_output("user"),
      },
    },
    {
      "fetch",
      elle::sprintf("Fetch a user from %s", beyond(true)),
      &fetch,
      {},
      {
        { "name,n", value<std::vector<std::string>>(),
          "user with name or hash to fetch" },
        { "no-avatar", bool_switch(), "do not fetch user avatar" },
      },
    },
    {
      "import",
      "Import a user",
      &import,
      {},
      {
        option_input("user"),
      },
    },
    {
      "pull",
      elle::sprintf("Remove a user from %s", beyond(true)),
      &pull,
      {},
      {
        { "name,n", value<std::string>(),
          "user to remove (default: system user)" },
        { "purge", bool_switch(), "remove objects owned by the user" },
      },
    },
    {
      "delete",
      "Delete a user locally",
      &delete_,
      {},
      {
        { "name,n", value<std::string>(),
          "user to delete (default: system user)" },
        { "force", bool_switch(), "delete the user without any prompt" },
        { "pull", bool_switch(),
          elle::sprintf("pull the user if it is on %s", beyond(true)) },
        { "purge", bool_switch(), "remove objects owned by the user" },
      },
    },
    {
      "push",
      elle::sprintf("Push a user to %s", beyond(true)),
      &push,
      {},
      {
        { "name,n", value<std::string>(),
          "user to push (default: system user)" },
        { "email", value<std::string>(), "valid email address" },
        option_fullname,
        option_avatar,
        option_push_full,
        option_push_password,
      },
    },
    {
      "signup",
      elle::sprintf("Create and push a user to %s", beyond(true)),
      &signup_,
      "--email EMAIL",
      {
        { "name,n", value<std::string>(), "user name (default: system user)" },
        option_description("user"),
        { "email", value<std::string>(), "valid email address" },
        option_fullname,
        option_avatar,
        option_key,
        option_push_full,
        option_push_password,
        option_ldap_dn,
      },
    },
    {
      "login",
      elle::sprintf("Log the user to %s", beyond(true)),
      &login,
      {},
      {
        { "name,n", value<std::string>(),
          "user name (default: system user)" },
        { "password", value<std::string>(), elle::sprintf(
          "password to authenticate with %s (default: prompt)",
          beyond(true)) },
      },
    },
    {
      "list",
      "List users",
      &list,
    },
  };
  Modes hidden_modes {
    {
      "hash",
      "Get short hash of user's key",
      &hash_,
      "--name USER",
      {
        { "name,n", value<std::string>(), "user name (default: system user)" },
      },
    },
  };
  return infinit::main(
    "Infinit user utility", modes, argc, argv, {}, {}, hidden_modes);
}
