#include <infinit/cli/User.hh>

#include <iostream>

#include <cryptography/rsa/pem.hh>

#include <reactor/http/url.hh>

#include <infinit/LoginCredentials.hh>
#include <infinit/cli/Infinit.hh>

ELLE_LOG_COMPONENT("cli.user");

namespace bfs = boost::filesystem;

namespace infinit
{
  namespace cli
  {
    using PublicUser = das::Model<
      infinit::User,
      decltype(elle::meta::list(
                 infinit::symbols::name,
                 infinit::symbols::description,
                 infinit::symbols::fullname,
                 infinit::symbols::public_key,
                 infinit::symbols::ldap_dn))>;

    using PublicUserPublish = das::Model<
      infinit::User,
      decltype(elle::meta::list(
                 infinit::symbols::name,
                 infinit::symbols::description,
                 infinit::symbols::email,
                 infinit::symbols::fullname,
                 infinit::symbols::public_key,
                 infinit::symbols::ldap_dn))>;

    User::User(Infinit& infinit)
      : Object(infinit)
      , create(
        "Create a user",
        das::cli::Options(),
        this->bind(modes::mode_create,
                   cli::name = Infinit::default_user_name(),
                   cli::description = boost::none,
                   cli::key = boost::none,
                   cli::email = boost::none,
                   cli::fullname = boost::none,
                   cli::password = boost::none,
                   cli::ldap_name = boost::none,
                   cli::output = boost::none,
                   cli::push_user = false,
                   cli::push = false,
                   cli::full = false))
      , delete_(
        "Delete local user",
        das::cli::Options(),
        this->bind(modes::mode_delete,
                   cli::name = Infinit::default_user_name(),
                   cli::pull = false,
                   cli::purge = false))
      , export_(
        "Export local user",
        das::cli::Options(),
        this->bind(modes::mode_export,
                   cli::name = Infinit::default_user_name(),
                   cli::full = false,
                   cli::output = boost::none))
      , fetch(
        "Fetch users from {hub}",
        das::cli::Options(),
        this->bind(modes::mode_fetch,
                   cli::name = Infinit::default_user_name(),
                   cli::no_avatar = false))
      , hash(
        "Get short hash of user's key",
        das::cli::Options(),
        this->bind(modes::mode_hash,
                   cli::name = Infinit::default_user_name()))
      , import(
        "Import local user",
        das::cli::Options(),
        this->bind(modes::mode_import,
                   cli::input = boost::none))
      , list(
        "List local users",
        das::cli::Options(),
        this->bind(modes::mode_list))
      , login(
        "Login user to {hub}",
        das::cli::Options(),
        this->bind(modes::mode_login,
                   cli::name = Infinit::default_user_name(),
                   cli::password = boost::none))
      , pull(
        "Pull a user from {hub}",
        das::cli::Options(),
        this->bind(modes::mode_pull,
                   cli::name = Infinit::default_user_name(),
                   cli::purge = false))
      , push(
        "Push a user from {hub}",
        das::cli::Options(),
        this->bind(modes::mode_push,
                   cli::name = Infinit::default_user_name(),
                   cli::email = boost::none,
                   cli::fullname = boost::none,
                   cli::password = boost::none,
                   cli::avatar = boost::none,
                   cli::full = false))
      , signup(
        "Create and push a user to {hub}",
        das::cli::Options(),
        this->bind(modes::mode_signup,
                   cli::name = Infinit::default_user_name(),
                   cli::description = boost::none,
                   cli::key = boost::none,
                   cli::email = boost::none,
                   cli::fullname = boost::none,
                   cli::password = boost::none,
                   cli::ldap_name = boost::none,
                   cli::full = false))
    {}

    namespace
    {
      template <typename Buffer>
      void
      save_avatar(User& api,
                  std::string const& name,
                  Buffer const& buffer)
      {
        bfs::ofstream f;
        infinit::Infinit::_open_write(
          f, api.cli().infinit()._user_avatar_path(name),
          name, "avatar", true, std::ios::out | std::ios::binary);
        f.write(reinterpret_cast<char const*>(buffer.contents()), buffer.size());
        api.cli().report_action("saved", "avatar", name, "locally");
      }

      void
      upload_avatar(User& api,
                    infinit::User& self,
                    bfs::path const& avatar_path)
      {
        bfs::ifstream icon;
        infinit::Infinit::_open_read(icon, avatar_path, self.name, "icon");
        auto s = std::string(
          std::istreambuf_iterator<char>{icon},
          std::istreambuf_iterator<char>{});
        elle::ConstWeakBuffer data(s.data(), s.size());
        auto url = elle::sprintf("users/%s/avatar", self.name);
        api.cli().infinit().beyond_push_data(
          url, "avatar", self.name, data, "image/jpeg", self);
        save_avatar(api, self.name, data);
      }

      void
      fetch_avatar(User& api, std::string const& name)
      {
        auto url = elle::sprintf("users/%s/avatar", name);
        auto request = api.cli().infinit().beyond_fetch_data(url, "avatar", name);
        if (request->status() == reactor::http::StatusCode::OK)
        {
          auto response = request->response();
          // XXX: Deserialize XML.
          if (response.size() == 0 || response[0] == '<')
            throw MissingResource(
              elle::sprintf("avatar for %s not found on %s", name, beyond()));
          save_avatar(api, name, response);
        }
      }

      void
      pull_avatar(User& api, infinit::User& self)
      {
        auto url = elle::sprintf("users/%s/avatar", self.name);
        api.cli().infinit().beyond_delete(url, "avatar", self.name, self);
      }

      infinit::User
      create_user(User& api,
                  std::string const& name,
                  boost::optional<std::string> keys_file,
                  boost::optional<std::string> email,
                  boost::optional<std::string> fullname,
                  boost::optional<std::string> ldap_name,
                  boost::optional<std::string> description)
      {
        if (email && !validate_email(*email))
          elle::err<CLIError>("invalid email address: %s", *email);
        auto keys = [&]
        {
          if (keys_file)
          {
            auto passphrase = Infinit::read_passphrase();
            return infinit::cryptography::rsa::pem::import_keypair(
                *keys_file, passphrase);
          }
          else
          {
            api.cli().report("generating RSA keypair");
            return infinit::cryptography::rsa::keypair::generate(2048);
          }
        }();
        return {name, keys, email, fullname, ldap_name, description};
      }

      void
      user_push(User& api,
                infinit::User& user,
                boost::optional<std::string> password,
                bool full)
      {
        if (full)
        {
          if (!password)
            password = Infinit::read_password();
          if (!user.ldap_dn)
            user.password_hash = Infinit::hub_password_hash(*password);
          api.cli().infinit().beyond_push<das::Serializer<PrivateUserPublish>>(
            "user", user.name, user, user);
        }
        else
        {
          if (password)
            elle::err<CLIError>
              ("password is only used when pushing a full user");
          api.cli().infinit().beyond_push<das::Serializer<PublicUserPublish>>(
            "user", user.name, user, user, !api.cli().script());
        }
      }
    }

    /*------.
    | Modes |
    `------*/

    void
    User::mode_create(std::string const& name,
                      boost::optional<std::string> description,
                      boost::optional<std::string> key,
                      boost::optional<std::string> email,
                      boost::optional<std::string> fullname,
                      boost::optional<std::string> password,
                      boost::optional<std::string> ldap_name,
                      boost::optional<std::string> path,
                      bool push_user,
                      bool push,
                      bool full)
    {
      ELLE_TRACE_SCOPE("create");
      push = push || push_user;
      if (!push)
      {
        if (ldap_name)
          elle::err<CLIError>(
            "LDAP can only be used with the Hub, add --push");
        if (full)
          elle::err<CLIError>(
            "--full can only be used with the Hub, add --push");
        if (password)
          elle::err<CLIError>(
            "--password can only be used with the Hub, add --push");
      }
      if (ldap_name && !full)
        elle::err<CLIError>("LDAP user creation requires --full");
      infinit::User user =
        create_user(*this, name, key, email, fullname, ldap_name, description);
      if (auto output = this->cli().get_output(path, false))
      {
        infinit::Infinit::save(*output, user);
        this->cli().report_exported(std::cout, "user", user.name);
      }
      else
      {
        this->cli().infinit().user_save(user);
        this->cli().report_action("generated", "user", name, "locally");
      }
      if (push)
        user_push(*this, user, password, full);
    }

    void
    User::mode_delete(std::string const& name,
                      bool pull,
                      bool purge)
    {
      ELLE_TRACE_SCOPE("delete");
      auto& ifnt = this->cli().infinit();
      auto user = ifnt.user_get(name);
      if (user.private_key && !this->cli().script())
      {
        std::string res;
        {
          std::cout
            << "WARNING: The local copy of the user's private key will be removed.\n"
            << "WARNING: You will no longer be able to perform actions on " << beyond() << "\n"
            << "WARNING: for this user.\n"
            << "\n"
            << "Confirm the name of the user you would like to delete: ";
          std::getline(std::cin, res);
        }
        if (res != user.name)
          elle::err("Aborting...");
      }
      if (pull)
      {
        try
        {
          auto self = this->cli().as_user();
          ifnt.beyond_delete("user", name, self, true, purge);
        }
        catch (MissingLocalResource const& e)
        {
          elle::err("unable to pull user, ensure the user has been set "
                    "using --as or INFINIT_USER");
        }
      }
      if (purge)
      {
        // XXX Remove volumes and drives that are on network owned by this user.
        // Currently only the owner of a network can create volumes/drives.
        for (auto const& drive_: ifnt.drives_get())
        {
          auto drive = drive_.name;
          if (ifnt.owner_name(drive) != user.name)
            continue;
          auto drive_path = ifnt._drive_path(drive);
          if (bfs::remove(drive_path))
            this->cli().report_action("deleted", "drive", drive, "locally");
        }
        for (auto const& volume_: ifnt.volumes_get())
        {
          auto volume = volume_.name;
          if (ifnt.owner_name(volume) != user.name)
            continue;
          auto volume_path = ifnt._volume_path(volume);
          if (bfs::remove(volume_path))
            this->cli().report_action("deleted", "volume", volume, "locally");
        }
        for (auto const& pair: ifnt.passports_get())
        {
          auto network = pair.first.network();
          if (ifnt.owner_name(network) != user.name
              && pair.second != user.name)
          {
            continue;
          }
          auto passport_path = ifnt._passport_path(network, pair.second);
          if (bfs::remove(passport_path))
            this->cli().report_action("deleted", "passport",
                                      elle::sprintf("%s: %s", network, pair.second),
                                      "locally");
        }
        for (auto const& network_: ifnt.networks_get(user))
        {
          auto network = network_.name;
          if (ifnt.owner_name(network) == user.name)
            ifnt.network_delete(network, user, true);
          else
          {
            ifnt.network_unlink(network, user);
            this->cli().report_action("unlinked", "network", network.name());
          }
        }
      }
      if (auto path = this->cli().avatar_path(name))
        bfs::remove(*path);
      auto path = ifnt._user_path(user.name);
      if (bfs::remove(path))
      {
        this->cli().report_action("deleted", "user", user.name, "locally");
      }
      else
      {
        elle::err("File for user could not be deleted: %s", path);
      }
    }

    void
    User::mode_export(std::string const& name,
                      bool full,
                      boost::optional<std::string> path)
    {
      ELLE_TRACE_SCOPE("export");
      auto user = this->cli().infinit().user_get(name);
      auto output = this->cli().get_output(path);
      auto avatar = this->cli().avatar_path(name);
      if (avatar)
        user.avatar_path = avatar->string();
      if (full)
      {
        if (!this->cli().script())
        {
          elle::fprintf(std::cerr, "WARNING: you are exporting the user \"%s\" "
                        "including the private key\n", name);
          elle::fprintf(std::cerr, "WARNING: anyone in possession of this "
                        "information can impersonate that user\n");
          elle::fprintf(std::cerr, "WARNING: if you mean to export your user for "
                        "someone else, remove the --full flag\n");
        }
        elle::serialization::json::serialize(user, *output, false);
      }
      else
      {
        elle::serialization::json::serialize<
          das::Serializer<infinit::User, PublicUser>>(user, *output, false);
      }
      this->cli().report_exported(std::cout, "user", user.name);
    }

    void
    User::mode_fetch(std::vector<std::string> const& user_names,
                     bool no_avatar)
    {
      ELLE_TRACE_SCOPE("fetch");
      for (auto const& name: user_names)
      {
        auto avatar = [&] () {
          if (!no_avatar)
          {
            try
            {
              fetch_avatar(*this, name);
            }
            catch (elle::Error const&)
            {}
          }
        };
        try
        {
          auto user = this->cli().infinit().beyond_fetch<infinit::User>(
            "user", reactor::http::url_encode(name));
          this->cli().infinit().user_save(std::move(user));
          avatar();
        }
        catch (ResourceAlreadyFetched const& e)
        {
          avatar();
          throw;
        }
      }
    }

    void
    User::mode_hash(std::string const& name)
    {
      ELLE_TRACE_SCOPE("hash");
      auto user = this->cli().infinit().user_get(name);
      auto key_hash = infinit::model::doughnut::short_key_hash(user.public_key);
      if (this->cli().script())
      {
        auto res = elle::json::Object
          {
            {name, key_hash},
          };
        elle::json::write(std::cout, res);
      }
      else
        elle::fprintf(std::cout, "%s: %s\n", name, key_hash);
    }

    void
    User::mode_import(boost::optional<std::string> const& path)
    {
      ELLE_TRACE_SCOPE("import");
      auto input = this->cli().get_input(path);
      auto user =
        elle::serialization::json::deserialize<infinit::User>(*input, false);
      this->cli().infinit().user_save(user);
      this->cli().report_imported("user", user.name);
    }

    void
    User::mode_list()
    {
      ELLE_TRACE_SCOPE("list");
      auto users = this->cli().infinit().users_get();
      boost::optional<infinit::User> self;
      try
      {
        self.emplace(this->cli().as_user());
      }
      catch (MissingLocalResource const&)
      {}
      std::sort(users.begin(), users.end(),
                [] (infinit::User const& lhs, infinit::User const& rhs)
                { return lhs.name < rhs.name; });
      if (this->cli().script())
      {
        auto l = elle::json::make_array(users, [](auto const& user) {
          auto res = elle::json::Object
            {
              {"name", static_cast<std::string>(user.name)},
              {"has_private_key",  bool(user.private_key)},
            };
          if (user.description)
            res["description"] = *user.description;
          return res;
          });
        elle::json::write(std::cout, l);
      }
      else
      {
        bool user_in_list = self && contains(users, self);
        for (auto const& user: users)
        {
          std::cout << (self && user == self ? "* " : user_in_list ? "  " : "")
                    << user.name;
          if (user.description)
            std::cout << " \"" << *user.description << "\"";
          std::cout << ": public";
          if (user.private_key)
            std::cout << "/private keys";
          else
            std::cout << " key only";
          std::cout << std::endl;
        }
      }
    }

    void
    User::mode_login(std::string const& name,
                     boost::optional<std::string> const& password)
    {
      ELLE_TRACE_SCOPE("login");
      auto pass = password.value_or(Infinit::read_password());
      auto hashed_pass = Infinit::hub_password_hash(pass);
      auto c = LoginCredentials{ name, hashed_pass, pass };
      auto json = this->cli().infinit().beyond_login(name, c);
      elle::serialization::json::SerializerIn input(json, false);
      auto user = input.deserialize<infinit::User>();
      this->cli().infinit().user_save(user, true);
      this->cli().report_action("saved", "user", name, "locally");
    }

    void
    User::mode_pull(std::string const& name, bool purge)
    {
      ELLE_TRACE_SCOPE("pull");
      auto self = this->cli().as_user();
      this->cli().infinit().beyond_delete("user", name, self, false, purge);
    }

    void
    User::mode_push(std::string const& name,
                    boost::optional<std::string> email,
                    boost::optional<std::string> fullname,
                    boost::optional<std::string> password,
                    boost::optional<std::string> avatar,
                    bool full)
    {
      ELLE_TRACE_SCOPE("push");
      auto user = this->cli().infinit().user_get(name);
      // FIXME: why does push provide a way to update those fields?
      if (email || fullname)
      {
        if (email)
          user.email = *email;
        if (fullname)
          user.fullname = *fullname;
        this->cli().infinit().user_save(user, true);
        this->cli().report_updated("user", user.name);
      }
      user_push(*this, user, password, full);
      // FIXME: avatar should probably be stored locally too
      if (avatar)
      {
        if (!avatar->empty())
        {
          if (!bfs::exists(*avatar))
            elle::err("avatar file doesn't exist: %s", *avatar);
          upload_avatar(*this, user, *avatar);
        }
        else
          pull_avatar(*this, user);
      }
    }

    void
    User::mode_signup(std::string const& name,
                      boost::optional<std::string> description,
                      boost::optional<std::string> key,
                      boost::optional<std::string> email,
                      boost::optional<std::string> fullname,
                      boost::optional<std::string> password,
                      boost::optional<std::string> ldap_name,
                      bool full)
    {
      ELLE_TRACE_SCOPE("signup");
      if (ldap_name && !full)
        elle::err<CLIError>("LDAP user creation requires --full");
      auto user = create_user(*this,
                              name,
                              key,
                              email,
                              fullname,
                              ldap_name,
                              description);
      auto user_exists = false;
      try
      {
        this->cli().infinit().user_get(name);
        user_exists = true;
      }
      catch (elle::Error const&)
      {
        user_push(*this, user, password, full);
        this->cli().infinit().user_save(user);
      }
      if (user_exists)
        elle::err("user %s already exists locally", name);
    }
  }
}
