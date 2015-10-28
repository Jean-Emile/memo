import bottle
import cryptography
import datetime
import hashlib
import json
import time

import Crypto.Hash.SHA
import Crypto.Hash.SHA256
import Crypto.PublicKey
import Crypto.Signature.PKCS1_v1_5

from copy import deepcopy
from requests import Request, Session

from infinit.beyond import *
from infinit.beyond.gcs import GCS


## -------- ##
## Response ##
## -------- ##

class Response(Exception):

  def __init__(self, status = 200, body = None):
    self.__status = status
    self.__body = body

  @property
  def status(self):
    return self.__status

  @property
  def body(self):
    return self.__body

class ResponsePlugin(object):

  '''Bottle plugin to generate throw a response.'''

  name = 'meta.short-circuit'
  api  = 2

  def apply(self, f, route):
    def wrapper(*args, **kwargs):
      try:
        return f(*args, **kwargs)
      except Response as response:
        bottle.response.status = response.status
        return response.body
    return wrapper


## ------ ##
## Bottle ##
## ------ ##

class Bottle(bottle.Bottle):

  __oauth_services = {
    'dropbox': {
      'form_url': 'https://www.dropbox.com/1/oauth2/authorize',
      'exchange_url': 'https://api.dropbox.com/1/oauth2/token',
      'info_url': 'https://api.dropbox.com/1/account/info',
      'info': lambda info: {
        'uid': str(info['uid']),
        'display_name': info['display_name'],
      },
    },
    'google': {
      'form_url': 'https://accounts.google.com/o/oauth2/auth',
      'exchange_url': 'https://www.googleapis.com/oauth2/v3/token',
      'params': {
        'scope': 'https://www.googleapis.com/auth/drive.file',
        'access_type': 'offline',
      },
      'info_url': 'https://www.googleapis.com/drive/v2/about',
      'info': lambda info: {
        'uid': info['user']['emailAddress'],
        'display_name': info['name'],
      },
    },
  }

  def __init__(
      self,
      beyond,
      gcs = None,
  ):
    super().__init__()
    self.__beyond = beyond
    self.install(bottle.CertificationPlugin())
    self.install(ResponsePlugin())
    self.route('/')(self.root)
    # GCS
    self.__gcs = gcs
    # OAuth
    for s in Bottle.__oauth_services:
      self.route('/oauth/%s' % s)(getattr(self, 'oauth_%s' % s))
      self.route('/users/<username>/%s-oauth' % s)(
        getattr(self, 'oauth_%s_get' % s))
      self.route('/users/<username>/credentials/%s' % s,
                 method = 'GET')(
        getattr(self, 'user_%s_credentials_get' % s))
    self.route('/users/<username>/credentials/google/refresh',
               method = 'GET')(
    getattr(self, 'user_credentials_google_refresh'))
    # User
    self.route('/users/<name>', method = 'GET')(self.user_get)
    self.route('/users/<name>', method = 'PUT')(self.user_put)
    self.route('/users/<name>', method = 'DELETE')(self.user_delete)
    self.route('/users/<name>/avatar', method = 'GET')(
      self.user_avatar_get)
    self.route('/users/<name>/avatar', method = 'PUT')(
      self.user_avatar_put)
    self.route('/users/<name>/avatar', method = 'DELETE')(
      self.user_avatar_delete)
    self.route('/users/<name>/networks', method = 'GET')(self.user_networks_get)
    # Network
    self.route('/networks/<owner>/<name>',
               method = 'GET')(self.network_get)
    self.route('/networks/<owner>/<name>',
               method = 'PUT')(self.network_put)
    self.route('/networks/<owner>/<name>',
               method = 'DELETE')(self.network_delete)
    self.route('/networks/<owner>/<name>/passports/<invitee>',
               method = 'GET')(self.network_passport_get)
    self.route('/networks/<owner>/<name>/passports/<invitee>',
               method = 'PUT')(self.network_passport_put)
    self.route('/networks/<owner>/<name>/endpoints/<user>/<node_id>',
               method = 'DELETE')(self.network_endpoint_delete)
    self.route('/networks/<owner>/<name>/endpoints',
               method = 'GET')(self.network_endpoints_get)
    self.route('/networks/<owner>/<name>/endpoints/<user>/<node_id>',
               method = 'PUT')(self.network_endpoint_put)
    self.route('/networks/<owner>/<name>/users',
               method = 'GET')(self.network_users_get)
    # Volume
    self.route('/volumes/<owner>/<name>',
               method = 'GET')(self.volume_get)
    self.route('/volumes/<owner>/<name>',
               method = 'PUT')(self.volume_put)
    self.route('/volumes/<owner>/<name>',
               method = 'DELETE')(self.volume_delete)

  def __not_found(self, type, name):
    return Response(404, {
      'error': '%s/not_found' % type,
      'reason': '%s %r does not exist' % (type, name),
      'name': name,
    })

  def __user_not_found(self, name):
    return self.__not_found('user', name)

  def authenticate(self, user):
    remote_signature_raw = bottle.request.headers.get('infinit-signature')
    if remote_signature_raw is None:
      raise Response(401, 'Missing signature header')
    request_time = bottle.request.headers.get('infinit-time')
    if request_time is None:
      raise Response(400, 'Missing time header')
    if abs(time.time() - int(request_time)) > 300: # UTC
      raise Response(401, 'Time too far away: got %s, current %s' % \
                     (request_time, time.time()))
    rawk = user.public_key['rsa']
    der = base64.b64decode(rawk.encode('latin-1'))
    k = Crypto.PublicKey.RSA.importKey(der)
    to_sign = bottle.request.method + ';' + bottle.request.path[1:] + ';'
    to_sign += base64.b64encode(
      hashlib.sha256(bottle.request.body.getvalue()).digest()).decode('utf-8') + ";"
    to_sign += request_time
    local_hash = Crypto.Hash.SHA256.new(to_sign.encode('utf-8'))
    remote_signature_crypted = base64.b64decode(remote_signature_raw.encode('utf-8'))
    verifier = Crypto.Signature.PKCS1_v1_5.new(k)
    if not verifier.verify(local_hash, remote_signature_crypted):
      raise Response(403, 'Authentication error')
    pass

  def root(self):
    return {
      'version': infinit.beyond.version.version,
    }

  def host(self):
    return '%s://%s' % bottle.request.urlparts[0:2]

  def debug(self):
    if hasattr(bottle.request, 'certificate') and \
       bottle.request.certificate in [
         'antony.mechin@infinit.io',
         'baptiste.fradin@infinit.io',
         'christopher.crone@infinit.io',
         'gaetan.rochel@infinit.io',
         'julien.quintard@infinit.io',
         'matthieu.nottale@infinit.io',
         'patrick.perlmutter@infinit.io',
         'quentin.hocquet@infinit.io',
       ]:
      return True
    else:
      return super().debug()

  ## ---- ##
  ## User ##
  ## ---- ##

  def user_put(self, name):
    try:
      json = bottle.request.json
      user = User.from_json(self.__beyond, json)
      user.create()
      bottle.response.status = 201
      return {}
    except User.Duplicate:
      if user.public_key == self.__beyond.user_get(user.name).public_key:
          bottle.response.status = 200
      else:
        bottle.response.status = 409
        return {
          'error': 'user/conflict',
          'reason': 'user %r already exists' % name,
          'id': name,
        }

  def user_get(self, name):
    try:
      return self.__beyond.user_get(name = name).json()
    except User.NotFound:
      raise self.__user_not_found(name)

  def user_delete(self, name):
    user = self.__beyond.user_get(name = name)
    self.authenticate(user)
    self.__beyond.user_delete(name)

  def user_avatar_put(self, name):
    return self.__user_avatar_manipulate(
      name, self.__cloud_image_upload)

  def user_avatar_get(self, name):
    return self.__user_avatar_manipulate(
      name, self.__cloud_image_get)

  def user_avatar_delete(self, name):
    return self.__user_avatar_manipulate(
      name, self.__cloud_image_delete)

  def __user_avatar_manipulate(self, name, f):
    return f('users', '%s/avatar' % name)

  def user_networks_get(self, name):
    try:
      user = self.__beyond.user_get(name = name)
    except User.NotFound:
      raise self.__user_not_found(name)
    self.authenticate(user)
    return {'networks': self.__beyond.user_networks_get(user = user)}

  ## ------- ##
  ## Network ##
  ## ------- ##

  def network_get(self, owner, name):
    try:
      return self.__beyond.network_get(
        owner = owner, name = name).json()
    except Network.NotFound:
      raise self.__not_found('network', '%s/%s' % (owner, name))

  def network_put(self, owner, name):
    try:
      user = self.__beyond.user_get(name = owner)
      self.authenticate(user)
      json = bottle.request.json
      network = Network(self.__beyond, **json)
      network.create()
      bottle.response.status = 201
      return {}
    except User.NotFound:
      raise self.__user_not_found(owner)
    except Network.Duplicate:
      bottle.response.status = 409
      return {
        'error': 'network/conflict',
        'reason': 'network %r already exists' % name,
      }

  def network_passport_get(self, owner, name, invitee):
    user = self.__beyond.user_get(name = owner)
    network = self.__beyond.network_get(
      owner = owner, name = name)
    passport = network.passports.get(invitee)
    if passport is None:
      raise self.__not_found(
        'passport', '%s/%s/%s' % (owner, name, invitee))
    else:
      return passport

  def network_passport_put(self, owner, name, invitee):
    try:
      user = self.__beyond.user_get(name = owner)
      try:
        self.authenticate(user)
      except Exception:
        u_invitee = self.__beyond.user_get(name = invitee)
        self.authenticate(u_invitee)

      network = Network(self.__beyond, owner = owner, name = name)
      network.passports[invitee] = bottle.request.json
      network.save()
      bottle.response.status = 201
      return {}
    except Network.NotFound:
      raise self.__not_found('network', '%s/%s' % (owner, name))

  def network_endpoints_get(self, owner, name):
    try:
      network = self.__beyond.network_get(owner = owner,
                                          name = name)
      return network.endpoints
    except Network.NotFound:
      raise self.__not_found('network', '%s/%s' % (owner, name))

  def network_endpoint_put(self, owner, name, user, node_id):
    try:
      user = self.__beyond.user_get(name = user)
      self.authenticate(user)
      network = Network(self.__beyond, owner = owner, name = name)
      json = bottle.request.json
      # FIXME
      # if 'port' not in json or 'addresses' not in json
      network.endpoints.setdefault(user.name, {})[node_id] = json
      network.save()
      bottle.response.status = 201 # FIXME: 200 if existed
      return {}
    except Network.NotFound:
      raise self.__not_found('network', '%s/%s' % (owner, name))

  def network_endpoint_delete(self, owner, name, user, node_id):
    try:
      user = self.__beyond.user_get(name = user)
      self.authenticate(user)
      network = Network(self.__beyond, owner = owner, name = name)
      network.endpoints.setdefault(user.name, {})[node_id] = None
      network.save()
      return {}
    except Network.NotFound:
      raise self.__not_found('network', '%s/%s' % (owner, name))

  def network_delete(self, owner, name):
    user = self.__beyond.user_get(name = owner)
    self.authenticate(user)
    self.__beyond.network_delete(owner, name)

  def network_users_get(self, owner, name):
    try:
      network = self.__beyond.network_get(owner = owner, name = name)
      res = [owner]
      res.extend(network.passports.keys())
      return {
        'users': res,
      }
    except Network.NotFound:
      raise self.__not_found('network', '%s\%s' % (owner, name))

  ## ------ ##
  ## Volume ##
  ## ------ ##

  def volume_get(self, owner, name):
    try:
      return self.__beyond.volume_get(
        owner = owner, name = name).json()
    except Volume.NotFound:
      raise self.__not_found('volume', '%s/%s' % (owner, name))

  def volume_put(self, owner, name):
    user = self.__beyond.user_get(name = owner)
    self.authenticate(user)
    try:
      json = bottle.request.json
      volume = Volume(self.__beyond, **json)
      volume.create()
    except Volume.Duplicate:
      bottle.response.status = 409
      return {
        'error': 'volume/conflict',
        'reason': 'volume %r already exists' % name,
      }

  def volume_delete(self, owner, name):
    user = self.__beyond.user_get(name = owner)
    self.authenticate(user)
    self.__beyond.volume_delete(owner = owner, name = name)

  ## --- ##
  ## GCS ##
  ## --- ##

  def __check_gcs(self):
    if self.__gcs is None:
      raise Response(501, {
        'reason': 'GCS support not enabled',
      })

  def __cloud_image_upload(self, bucket, name):
    self.__check_gcs()
    t = bottle.request.headers['Content-Type']
    l = bottle.request.headers['Content-Length']
    if t not in ['image/gif', 'image/jpeg', 'image/png']:
      bottle.response.status = 415
      return {
        'reason': 'invalid image format: %s' % t,
        'mime-type': t,
      }
    url = self.__gcs.upload_url(
      bucket,
      name,
      content_type = t,
      content_length = l,
      expiration = datetime.timedelta(minutes = 3),
    )
    bottle.response.status = 307
    bottle.response.headers['Location'] = url

  def __cloud_image_get(self, bucket, name):
    self.__check_gcs()
    url = self.__gcs.download_url(
      bucket,
      name,
      expiration = datetime.timedelta(minutes = 3),
    )
    bottle.response.status = 307
    bottle.response.headers['Location'] = url

  def __cloud_image_delete(self, bucket, name):
    self.__check_gcs()
    url = self.__gcs.delete_url(
      bucket,
      name,
      expiration = datetime.timedelta(minutes = 3),
    )
    bottle.response.status = 307
    bottle.response.headers['Location'] = url
    # self.gcs.delete(
    #   bucket,
    #   name)
    # bottle.response.status = 204

for name, conf in Bottle._Bottle__oauth_services.items():
  def oauth_get(self, username, name = name, conf = conf):
    beyond = self._Bottle__beyond
    params = {
      'client_id': getattr(beyond, '%s_app_key' % name),
      'response_type': 'code',
      'redirect_uri': '%s/oauth/%s' % (self.host(), name),
      'state': username,
    }
    if name == 'google':
      params['approval_prompt'] = 'force'
    params.update(conf.get('params', {}))
    req = requests.Request('GET', conf['form_url'], params = params)
    url = req.prepare().url
    bottle.redirect(url)
  oauth_get.__name__ = 'oauth_%s_get' % name
  setattr(Bottle, oauth_get.__name__, oauth_get)
  def oauth(self, name = name, conf = conf):
    beyond = self._Bottle__beyond
    code = bottle.request.query['code']
    username = bottle.request.query['state']
    query = {
      'code': code,
      'grant_type': 'authorization_code',
      'client_id':
        getattr(self._Bottle__beyond, '%s_app_key' % name),
      'client_secret':
        getattr(self._Bottle__beyond, '%s_app_secret' % name),
      'redirect_uri': '%s/oauth/%s' % (self.host(), name),
    }
    response = requests.post(conf['exchange_url'], params = query)
    if response.status_code // 100 != 2:
      bottle.response.status = response.status_code
      return response.text
    contents = response.json()
    access_token = contents['access_token']
    if 'refresh_token' in contents:
      refresh_token = contents['refresh_token']
    else:
      refresh_token = ''
    user = User(beyond, name = username)
    response = requests.get(
      conf['info_url'], params = {'access_token': access_token})
    if response.status_code // 100 != 2:
      bottle.response.status = response.status_code
      return response.text
    info = conf['info'](response.json())
    getattr(user, '%s_accounts' % name)[info['uid']] = \
      dict(info, token = access_token, refresh_token = refresh_token)
    try:
      user.save()
      return info
    except User.NotFound:
      raise self.__user_not_found(username)
  oauth.__name__ = 'oauth_%s' % name
  setattr(Bottle, oauth.__name__, oauth)
  def user_credentials_get(self, username, name = name):
    beyond = self._Bottle__beyond
    try:
      user = beyond.user_get(name = username)
      self.authenticate(user)
      return {
        'credentials':
          list(getattr(user, '%s_accounts' % name).values()),
      }
    except User.NotFound:
      raise self.__user_not_found(username)
  user_credentials_get.__name__ = 'user_%s_credentials_get' % name
  setattr(Bottle, user_credentials_get.__name__, user_credentials_get)

# This function first checks if the google account `token` field is
# valid.  If not it asks google for another access_token and updates
# the client, else it return to the client the access_token of the
# database.
def user_credentials_google_refresh(self, username):
  try:
    beyond = self._Bottle__beyond
    user = beyond.user_get(name = username)
    refresh_token = bottle.request.query.refresh_token
    for id, account in user.google_accounts.items():
      google_account = user.google_accounts[id]
      # https://developers.google.com/identity/protocols/OAuth2InstalledApp
      # The associate google account.
      if google_account['refresh_token'] == refresh_token:
        google_url = "https://www.googleapis.com/oauth2/v3/token"
        # Get a new token and update the db and the client
        query = {
          'client_id': beyond.google_app_key,
          'client_secret': beyond.google_app_secret,
          'refresh_token': google_account['refresh_token'],
          'grant_type': 'refresh_token',
        }
        res = requests.post(google_url, params=query)
        if res.status_code != 200:
          raise HTTPError(status=400)
        else:
          token = res.json()['access_token']
          user.google_accounts[id]['token'] = token
          user.save()
          return token
  except User.NotFound:
    raise self.__user_not_found(username)

setattr(Bottle,
        user_credentials_google_refresh.__name__,
        user_credentials_google_refresh)
