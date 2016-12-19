#include <algorithm>
#include <iterator>

#include <elle/json/json.hh>
#include <elle/log.hh>

// Must be placed before main.hh.
ELLE_LOG_COMPONENT("infinit-drive");

#include <main.hh>
#include <email.hh>

infinit::Infinit ifnt;

using boost::program_options::variables_map;

namespace
{
  std::string
  drive_name(variables_map const& args, infinit::User const& owner)
  {
    return ifnt.qualified_name(mandatory(args, "name"), owner);
  }

  boost::optional<boost::filesystem::path>
  icon_path(std::string const& name)
  {
    auto path = ifnt._drive_icon_path(name);
    if (boost::filesystem::exists(path))
      return path;
    else
      return {};
  }

  template <typename Buffer>
  void
  _save_icon(std::string const& name,
             Buffer const& buffer)
  {
    boost::filesystem::ofstream f;
    ifnt._open_write(f, ifnt._drive_icon_path(name),
                     name, "icon", true, std::ios::out | std::ios::binary);
    f.write(reinterpret_cast<char const*>(buffer.contents()), buffer.size());
    report_action("fetched", "icon", name, std::string("locally"));
  }

  void
  upload_icon(infinit::User& self,
              infinit::Drive& drive,
              boost::filesystem::path const& icon_path)
  {
    boost::filesystem::ifstream icon;
    ifnt._open_read(icon, icon_path, self.name, "icon");
    std::string s(
      std::istreambuf_iterator<char>{icon},
      std::istreambuf_iterator<char>{});
    elle::ConstWeakBuffer data(s.data(), s.size());
    auto url = elle::sprintf("drives/%s/icon", drive.name);
    beyond_push_data(url, "icon", drive.name, data, "image/jpeg", self);
    _save_icon(drive.name, data);
  }

  void
  pull_icon(infinit::User& self,
            infinit::Drive& drive)
  {
    auto url = elle::sprintf("drives/%s/icon", drive.name);
    beyond_delete(url, "icon", drive.name, self);
  }

  void
  _push(variables_map const& args,
        infinit::User& user,
        infinit::Drive& drive)
  {
    auto icon_path = optional(args, "icon");
    if (icon_path && icon_path.get().length() > 0)
    {
      if (!boost::filesystem::exists(icon_path.get()))
        throw CommandLineError(
          elle::sprintf("%s doesn't exist", icon_path.get()));
    }
    auto url = elle::sprintf("drives/%s", drive.name);
    beyond_push(url, "drive", drive.name, drive, user);
    if (icon_path)
    {
      if (icon_path.get().length() > 0)
        upload_icon(user, drive, icon_path.get());
      else
        pull_icon(user, drive);
    }
  }
}

COMMAND(create)
{
  ELLE_TRACE_SCOPE("create");
  auto owner = self_user(ifnt, args);
  auto name = drive_name(args, owner);
  auto network = ifnt.network_get(mandatory(args, "network"), owner);
  infinit::Volume volume(
    ifnt.volume_get(ifnt.qualified_name(mandatory(args, "volume"), owner)));
  infinit::Drive::Users users;
  infinit::Drive drive{
    name, owner, volume, network, optional(args, "description"), users};
  ifnt.drive_save(drive);
  report_action("created", "drive", drive.name, std::string("locally"));

  if (aliased_flag(args, {"push-drive", "push"}))
    _push(args, owner, drive);
}

namespace
{
  void
  push_passport(infinit::Network const& network,
                infinit::Passport const& passport,
                infinit::User const& user,
                infinit::User const& owner)
  {
    auto url = elle::sprintf("networks/%s/passports/%s", network.name, user.name);
    beyond_push(url,
                "passport",
                elle::sprintf("%s: %s", network.name, user.name),
                passport,
                owner);
  }

  void
  create_passport(infinit::User const& user,
                  infinit::Network const& network,
                  infinit::User const& owner,
                  bool push)
  {
    ELLE_TRACE_SCOPE("create_passport");
    try
    {
      auto passport = ifnt.passport_get(network.name, user.name);
      ELLE_DEBUG("passport (%s: %s) found", network.name, user.name);
      if (push)
        push_passport(network, passport, user, owner);
    }
    catch (infinit::MissingResource const& e)
    {
      auto passport = infinit::Passport(
        user.public_key,
        network.name,
        infinit::cryptography::rsa::KeyPair(owner.public_key,
                                            owner.private_key.get()));
      ELLE_DEBUG("passport (%s: %s) created", network.name, user.name);
      ifnt.passport_save(passport);
      report_created("passport",
                     elle::sprintf("%s: %s", network.name, user.name));
      if (push)
        push_passport(network, passport, user, owner);
    }
  }

  /*
   *  Compare the current drive json's invitee node with argument invitations.
   *  Add non-existing users.
   */
  void
  _update_local_json(
    infinit::Drive& drive,
    std::unordered_map<std::string, infinit::Drive::User> const& invitations)
  {
    for (auto const& invitation: invitations)
    {
      if (drive.owner == invitation.first)
        continue;

      auto it = drive.users.find(invitation.first);
      if (it != drive.users.end())
        continue;

      drive.users[invitation.first] = invitation.second;
      report_action("created", "invitation",
                    elle::sprintf("%s: %s", drive.name, invitation.first),
                    std::string("locally"));
    }
    ifnt.drive_save(drive);
  }
}

COMMAND(invite)
{
  ELLE_TRACE_SCOPE("invite");
  auto owner = self_user(ifnt, args);
  auto name = drive_name(args, owner);
  auto home = flag(args, "home");
  auto emails = optional<std::vector<std::string>>(args, "email");
  auto users = optional<std::vector<std::string>>(args, "user");
  bool push = aliased_flag(args, { "push-invitations", "push" });
  ELLE_DEBUG("push: %s", push);
  if (!emails && !users && !push)
    throw CommandLineError("specify users using --user and/or --email");
  bool generate_passports = flag(args, "passport");
  ELLE_DEBUG("generate passports: %s", generate_passports);
  if (emails)
  {
    for (auto const& email: emails.get())
      valid_email(email);
  }

  if (aliased_flag(args, { "fetch", "fetch-drive" }))
  {
    try
    {
      auto url = elle::sprintf("/drives/%s", name);
      auto drive = ifnt.drive_fetch(name);
      ifnt.drive_save(drive, true);
    }
    catch (infinit::MissingResource const& e)
    {
      if (e.what() != std::string("drive/not_found"))
        throw e;
      // The drive has not been pushed yet. No need to sync.
    }
  }

  auto drive = ifnt.drive_get(name);
  auto volume = ifnt.volume_get(drive.volume);
  auto network = ifnt.network_get(drive.network, owner);
  auto permissions =
    volume.default_permissions ? volume.default_permissions.get() : "none";

  std::unordered_map<std::string, infinit::Drive::User> invitees;
  if (!users && !emails)
  {
    for (auto const& u: drive.users)
      invitees[u.first] = u.second;
  }
  if (users)
  {
    for (auto const& user_name: users.get())
      invitees[user_name] = {permissions, "pending", home};
  }
  if (emails)
  {
    // Ensure that the user has the passport for Beyond.
    static const std::string error_msg = elle::sprintf(
      "ERROR: In order to invite users by email, you must create a passport "
      "for \"%s\"\nwith the --allow-create-passport option.\nYou must then add "
      "the user to the DHT using infinit-acl --register\n\n",
      infinit::beyond_delegate_user());
    try
    {
      auto delegate_passport =
        ifnt.passport_get(network.name, infinit::beyond_delegate_user());
      if (!delegate_passport.allow_sign())
      {
        elle::fprintf(std::cerr, error_msg);
        elle::err("Missing --allow-create-passport flag for %s",
                  infinit::beyond_delegate_user());
      }
    }
    catch (infinit::MissingResource const& e)
    {
      elle::fprintf(std::cerr, error_msg);
      throw;
    }
    for (auto const& email: emails.get())
      invitees[email] = {permissions, "pending", home};
  }

  for (auto const& invitee: invitees)
  {
    // Email invitees do not need passports.
    if (valid_email(invitee.first))
      continue;
    auto user = ifnt.user_get(invitee.first);
    if (generate_passports)
    {
      create_passport(user, network, owner, push);
    }
    else
    {
      // Ensure that the user has a passport.
      auto passport = ifnt.passport_get(network.name, user.name);
      if (push)
        push_passport(network, passport, user, owner);
    }
  }

  if (users || emails)
    _update_local_json(drive, invitees);

  if (push)
  {
    auto url = elle::sprintf("drives/%s/invitations", drive.name);
    for (auto const& invitee: invitees)
    {
      try
      {
        beyond_push(
          elle::sprintf("drives/%s/invitations/%s",drive.name, invitee.first),
          "invitation",
          elle::sprintf("%s: %s", drive.name, invitee.first),
          drive.users[invitee.first],
          owner,
          true,
          true);
      }
      catch (infinit::BeyondError const& e)
      {
        if (e.error() == std::string("user/not_found"))
          not_found(e.name_opt(), "User");
        else if (e.error() == std::string("network/not_found"))
          not_found(e.name_opt(), "Network");
        else if (e.error() == std::string("volume/not_found"))
          not_found(e.name_opt(), "Volume");
        else if (e.error() == std::string("drive/not_found"))
          not_found(e.name_opt(), "Drive");
        else if (e.error() == std::string("passport/not_found"))
          not_found(e.name_opt(), "Passport");
        throw;
      }
    }
  }
}

COMMAND(join)
{
  ELLE_TRACE_SCOPE("join");
  auto self = self_user(ifnt, args);
  auto drive = ifnt.drive_get(drive_name(args, self));
  if (self.name == boost::filesystem::path(drive.name).parent_path().string())
    elle::err("The owner is automatically invited to its drives");
  auto it = drive.users.find(self.name);
  if (it == drive.users.end())
    elle::err("You haven't been invited to join %s", drive.name);
  auto invitation = it->second;
  invitation.status = "ok";
  auto url = elle::sprintf("drives/%s/invitations/%s", drive.name, self.name);
  try
  {
    beyond_push(url, "invitation", drive.name, invitation, self, false);
    report_action("joined", "drive", drive.name);
  }
  catch (infinit::MissingResource const& e)
  {
    if (e.what() == std::string("user/not_found"))
      not_found(self.name, "User"); // XXX: It might be the owner or you.
    else if (e.what() == std::string("drive/not_found"))
      not_found(drive.name, "Drive");
    throw;
  }
  drive.users[self.name] = invitation;
  ELLE_DEBUG("save drive %s", drive)
    ifnt.drive_save(drive);
}

COMMAND(export_)
{
  ELLE_TRACE_SCOPE("export");
  auto owner = self_user(ifnt, args);
  auto output = get_output(args);
  auto name = drive_name(args, owner);
  auto drive = ifnt.drive_get(name);
  auto icon = icon_path(name);
  if (icon)
    drive.icon_path = icon.get().string();
  elle::serialization::json::serialize(drive, *output, false);
  report_exported(*output, "drive", drive.name);
}

COMMAND(push)
{
  ELLE_TRACE_SCOPE("push");
  auto owner = self_user(ifnt, args);
  auto name = drive_name(args, owner);
  auto drive = ifnt.drive_get(name);
  _push(args, owner, drive);
}

COMMAND(delete_)
{
  ELLE_TRACE_SCOPE("delete");
  auto owner = self_user(ifnt, args);
  auto name = drive_name(args, owner);
  auto path = ifnt._drive_path(name);
  if (flag(args, "pull"))
    beyond_delete("drive", name, owner, true);
  if (flag(args, "purge"))
  { /* Nothing depends on a drive. */ }
  if (boost::filesystem::remove(path))
    report_action("deleted", "drive", name, std::string("locally"));
  else
    throw infinit::MissingLocalResource(
      elle::sprintf("File for drive could not be deleted: %s", path));
}

COMMAND(pull)
{
  ELLE_TRACE_SCOPE("pull");
  auto owner = self_user(ifnt, args);
  auto name = drive_name(args, owner);
  beyond_delete("drive", name, owner, false, flag(args, "purge"));
}

COMMAND(list)
{
  ELLE_TRACE_SCOPE("list");
  auto self = self_user(ifnt, args);
  if (script_mode)
  {
    elle::json::Array l;
    for (auto& drive: ifnt.drives_get())
    {
      elle::json::Object o;
      o["name"] = static_cast<std::string>(drive.name);
      if (drive.users.find(self.name) != drive.users.end())
        o["status"] = drive.users[self.name].status;
      if (drive.description)
        o["description"] = drive.description.get();
      l.push_back(std::move(o));
    }
    elle::json::write(std::cout, l);
  }
  else
    for (auto& drive: ifnt.drives_get())
    {
      std::cout << drive.name;
      if (drive.description)
        std::cout << " \"" << drive.description.get() << "\"";
      if (drive.users.find(self.name) != drive.users.end())
        std::cout << ": " << drive.users[self.name].status;
      std::cout << std::endl;
    }
}

namespace
{
  void
  fetch_(std::string const& drive_name)
  {
    ELLE_TRACE_SCOPE("fetch %s", drive_name);
    auto remote_drive = ifnt.drive_fetch(drive_name);
    ELLE_DEBUG("save drive %s", remote_drive)
      ifnt.drive_save(remote_drive);
  }
}

COMMAND(fetch)
{
  ELLE_TRACE_SCOPE("fetch");
  auto self = self_user(ifnt, args);
  if (optional(args, "name"))
  {
    ELLE_DEBUG("fetch specific drive");
    auto name = drive_name(args, self);
    fetch_(name);
  }
  else
  {
    ELLE_DEBUG("fetch all drives");
    auto res = infinit::beyond_fetch<
      std::unordered_map<std::string, std::vector<infinit::Drive>>>(
        elle::sprintf("users/%s/drives", self.name),
        "drives for user",
        self.name,
        self);
    for (auto const& drive: res["drives"])
    {
      ifnt.drive_save(drive);
    }
  }
}

namespace
{
  void
  fetch_icon(std::string const& name)
  {
    auto url = elle::sprintf("drives/%s/icon", name);
    auto request = infinit::beyond_fetch_data(url, "icon", name);
    if (request->status() == reactor::http::StatusCode::OK)
    {
      auto response = request->response();
      // XXX: Deserialize XML.
      if (response.size() == 0 || response[0] == '<')
        throw infinit::MissingResource(
          elle::sprintf(
            "icon for %s not found on %s", name, infinit::beyond(true)));
      _save_icon(name, response);
    }
  }
}

int
main(int argc, char** argv)
{
  using boost::program_options::value;
  using boost::program_options::bool_switch;
  Mode::OptionDescription option_icon =
    { "icon,i", value<std::string>(), "path to an image to use as icon"};
  Modes modes {
    {
      "create",
      "Create a drive (a network and volume pair)",
      &create,
      "--name NAME --network NETWORK --volume VOLUME "
      "[--description DESCRIPTION]",
      {
        { "name,n", value<std::string>(), "created drive name" },
        option_description("drive"),
        { "network,N", value<std::string>(), "associated network name" },
        { "volume,V", value<std::string>(), "associated volume name" },
        option_icon,
        { "push-drive", bool_switch(),
          elle::sprintf("push the created drive to %s", infinit::beyond(true)) },
        { "push,p", bool_switch(), "alias for --push-drive" },
      },
    },
    {
      "invite",
      "Invite a user to join the drive",
      &invite,
      "--name DRIVE --user USER",
      {
        { "name,n", value<std::string>(), "drive to invite the user to" },
        { "user,u", value<std::vector<std::string>>()->multitoken(),
          "existing users to invite to the drive via user name" },
        { "email,e", value<std::vector<std::string>>()->multitoken(),
          "new users to invite to the drive via email" },
        { "fetch-drive", bool_switch(), "update local drive descriptor" },
        { "fetch,f", bool_switch(), "alias for --fetch-drive" },
        { "push-invitations", bool_switch(),
          "update remote drive descriptor and send invitations" },
        { "push,p", bool_switch(), "alias for --push-invitations" },
        { "passport", bool_switch(), "create passports for each invitee" },
      },
      {},
      // Hidden options.
      {
        { "permissions", value<std::string>(),
            "set default user permissions to XXX" },
        { "home,h", bool_switch(),
          "creates a home directory for the invited user" },
      },
    },
    {
      "join",
      "Join a drive you were invited to (Hub operation)",
      &join,
      "--name DRIVE",
      {
        { "name,n", value<std::string>(), "drive to invite the user on" },
      },
    },
    {
      "export",
      "Export a drive",
      &export_,
      "--name DRIVE",
      {
        { "name,n", value<std::string>(), "drive to export" },
      },
    },
    {
      "push",
      elle::sprintf("Push a drive to %s", infinit::beyond(true)),
      &push,
      "--name NAME",
      {
        { "name,n", value<std::string>(),
          elle::sprintf("drive to push to %s", infinit::beyond(true)) },
        option_icon,
      }
    },
    {
      "fetch",
      elle::sprintf("fetch drive from %s", infinit::beyond(true)),
      &fetch,
      "",
      {
        { "name,n", value<std::string>(), "drive to fetch (optional)" },
      },
    },
    {
      "list",
      "List drives",
      &list,
      {},
    },
    {
      "delete",
      "Delete a drive locally",
      &delete_,
      "--name NAME",
      {
        { "name,n", value<std::string>(), "drive to delete" },
        { "pull", bool_switch(),
          elle::sprintf("pull the drive if it is on %s", infinit::beyond(true)) },
        { "purge", bool_switch(), "remove objects that depend on the drive" },
      },
    },
    {
      "pull",
      elle::sprintf("Remove a drive from %s", infinit::beyond(true)),
      &pull,
      "--name NAME",
      {
        { "name,n", value<std::string>(), "drive to remove" },
        { "purge", bool_switch(), "remove objects that depend on the drive" },
      },
    },
  };
  return infinit::main("Infinit drive management utility", modes, argc, argv);
}
