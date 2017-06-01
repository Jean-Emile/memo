import bottle
import sendwithus
import requests
import os
import re
import json

from bottle import redirect
from infinit.website.utils import \
  error_page, \
  resources_path, \
  route, \
  static_file, \
  view, \
  detect_os

def error(code, reason = ''):
  bottle.response.status = code
  if not reason:
    reason = requests.status_codes._codes.get(code, '')[0]
  return {
    'reason': reason
  }

class Website(bottle.Bottle):

  def __init__(self):
    super().__init__()
    self.install(bottle.CertificationPlugin())
    route.apply(self)
    error_page.apply(self)
    self.__swu = sendwithus.api(api_key = 'live_f237084a19cbf6b2373464481155d953a4d86e8d')
    self.__hub = os.environ.get('INFINIT_BEYOND', 'https://beyond.infinit.sh')
    self.platforms = {}
    with open(resources_path() + '/json/platform/windows.json',
              encoding = 'utf-8') as json_file:
      self.platforms['windows'] = json.load(json_file)
    with open(resources_path() + '/json/platform/linux.json',
              encoding = 'utf-8') as json_file:
      self.platforms['linux'] = json.load(json_file)

  def __call__(self, e, h):
    e['PATH_INFO'] = e['PATH_INFO'].rstrip('/')
    return super().__call__(e, h)

  def debug(self):
    if hasattr(bottle.request, 'certificate') and \
       bottle.request.certificate in [
         'antony.mechin@infinit.io',
         'baptiste.fradin@infinit.io',
         'christopher.crone@infinit.io',
         'gaetan.rochel@infinit.io',
         'julien.quintard@infinit.io',
         'matthieu.nottale@infinit.io',
         'quentin.hocquet@infinit.io',
       ]:
      return True
    else:
      return super().debug()

  @route('/', name = 'home')
  @view('pages/home')
  def root(self):
    return {
      'description': 'Infinit allows for the creation of flexible, secure and controlled file storage infrastructure on top of public, private or hybrid cloud resources.',
    }

  @error_page(404)
  @view('pages/404')
  def root(self):
    return {
      'title': '404',
      'description': 'The page you were looking for isn\'t here.',
    }

  @route('/product', name = 'product')
  @view('pages/product.html')
  def root(self):
    return {
      'title': 'Product',
      'description': 'Discover the benefits of the Infinit decentralized storage platform: scalabilty, resilience and more.',
    }

  @route('/project/memo', name = 'memo')
  @view('pages/projects/memo.html')
  def root(self):
    return {
      'title': 'Memo Key-Value Store',
      'description': 'Modern key-value store built with replication and security in mind',
    }

  @route('/docker', name = 'docker')
  @view('pages/docker.html')
  def root(self):
    return {
      'title': 'Persistent Storage Solutions for Docker',
      'description': 'Infinit provides persistent storage through several solutions well suited to make your containerized Docker applications stateful.',
    }

  @route('/desktop', name = 'drive')
  @view('pages/desktop/desktop.html')
  def root(self):
    return {
      'title': 'Infinit Desktop',
      'description': 'Infinit Desktop allows any small and medium business to securely store and access files from anywhere through an easy-to-use virtual disk drive interface.',
    }

  @route('/desktop/linux', name = 'desktop_linux')
  @view('pages/desktop/desktop.html')
  def root(self):
    return {
      'title': 'Infinit Desktop - Linux',
      'description': 'Infinit Desktop allows any small and medium business to securely store and access files from anywhere through an easy-to-use virtual disk drive interface.',
    }

  @route('/desktop/mac', name = 'desktop_mac')
  @view('pages/desktop/desktop.html')
  def root(self):
    return {
      'title': 'Infinit Desktop - Mac',
      'description': 'The Infinit Desktop allows any small and medium business to securely store and access files from anywhere through an easy-to-use virtual disk drive interface.',
    }

  @route('/desktop/windows', name = 'desktop_windows')
  @view('pages/desktop/desktop.html')
  def root(self):
    return {
      'title': 'Infinit Desktop - Windows',
      'description': 'Infinit Desktop allows any small and medium business to securely store and access files from anywhere through an easy-to-use virtual disk drive interface.',
    }

  @route('/download', name = 'download')
  @view('pages/download.html')
  def root(self):
    return {
      'title': 'Download',
      'description': 'Download the Infinit command line tools for Mac, Windows or Linux (Ubuntu, Debian, CentOS, Alpine, Raspberry).',
    }

  @route('/update', name = 'update')
  @view('pages/update.html')
  def root(self):
    return {
      'title': 'Update',
      'description': 'Update your Infinit command line tools to the latest version to get all the new features.',
    }

  @route('/faq', name = 'faq')
  @view('pages/faq.html')
  def root(self):
    return {
      'title': 'FAQ',
      'description': 'Frequently Asked Questions about how to use our storage platform, how it compares to others and more.',
    }

  @route('/get-started', name = 'doc_get_started')
  @view('pages/docs/get_started.html')
  def root(self):
    return {
      'title': 'Get Started',
      'description': 'A step by step guide to getting started with the Infinit storage platform.',
      **self.platforms[detect_os() == "Windows" and 'windows' or 'linux']
    }

  @route('/get-started/mac', name = 'doc_get_started_mac')
  @view('pages/docs/get_started.html')
  def root(self):
    return {
      'title': 'Get Started for Mac',
      'description': 'A step by step guide to getting started with the Infinit storage platform.',
    }

  @route('/get-started/windows', name = 'doc_get_started_windows')
  @view('pages/docs/get_started.html')
  def root(self):
    return {
      'title': 'Get Started for Windows',
      'description': 'A step by step guide for Windows to getting started with the Infinit storage platform.',
    }

  @route('/get-started/linux', name = 'doc_get_started_linux')
  @view('pages/docs/get_started.html')
  def root(self):
    return {
      'title': 'Get Started for Linux',
      'description': 'A step by step guide for linux to getting started with the Infinit storage platform.',
    }

  @route('/documentation/technology', name = 'doc_technology')
  @view('pages/docs/technology.html')
  def root(self):
    return {
      'title': 'Technology Behind Infinit',
      'description': 'Discover the different layers composing the Infinit technology, from the reactor, the distributed hash table up to the file system.',
    }

  @route('/documentation/deployments', name = 'doc_deployments')
  @view('pages/docs/deployments.html')
  def root(self):
    return {
      'title': 'Examples of Deployments with Infinit',
      'description': 'Discover how Infinit can be used to deploy various types of storage infrastructure.',
    }

  @route('/documentation', name = 'doc_reference')
  @route('/documentation/reference', name = 'doc_reference')
  @view('pages/docs/reference.html')
  def root(self):
    return {
      'title': 'Reference Documentation',
      'description': 'Read the reference document detailing how to use the Infinit command-line tools, hub and more.',
    }

  @route('/documentation/roadmap', name = 'doc_roadmap')
  @view('pages/docs/roadmap.html')
  def root(self):
    return {
      'title': 'Roadmap',
      'description': 'Discover the next developments of the Infinit platform.',
    }

  @route('/documentation/changelog', name = 'doc_changelog')
  @view('pages/docs/changelog.html')
  def root(self):
    return {
      'title': 'Change Log',
      'description': 'Have a look at all the recent changes of the Infinit platform.',
    }

  @route('/documentation/changelog/<path:path>')
  def root(self, path):
    redirect('/documentation/changelog#' + path)

  @route('/documentation/status', name = 'doc_status')
  @view('pages/docs/status.html')
  def root(self):
    return {
      'title': 'Status',
      'description': 'Keep track of announcements regarding system wide issues or performance status.',
    }

  @route('/documentation/storages/filesystem', name = 'doc_storages_filesystem')
  @view('pages/docs/filesystem.html')
  def root(self):
    return {
      'title': 'Filesystem storage',
      'description': 'Create a storage resource that uses a local filesystem folder.',
    }

  @route('/documentation/storages/gcs', name = 'doc_storages_gcs')
  @view('pages/docs/gcs.html')
  def root(self):
    return {
      'title': 'Google Cloud Storage',
      'description': 'Create a storage resource that uses GCS bucket.',
    }

  @route('/documentation/storages/s3', name = 'doc_storages_s3')
  @view('pages/docs/s3.html')
  def root(self):
    return {
      'title': 'Amazon S3 Storage',
      'description': 'Create a storage resource that uses an Amazon S3 bucket.',
    }

  @route('/documentation/docker/volume-plugin', name = 'doc_docker_plugin')
  @view('pages/docs/docker_volume_plugin.html')
  def root(self):
    return {
      'title': 'Docker Volume Plugin',
      'description': 'The Infinit Docker volume plugins enable Engine deployments to be integrated with Infinit volumes and enable data volumes to persist beyond the lifetime of a single Engine host. ',
    }

  @route('/documentation/upgrading', name = 'doc_upgrading')
  @view('pages/docs/upgrading.html')
  def root(self):
    return {
      'title': 'Upgrade Network',
      'description': 'How to upgrade an Infinit network for all the clients to benefit from new features.',
    }

  @route('/documentation/environment-variables', name = 'doc_environment_variables')
  @view('pages/docs/environment_variables.html')
  def root(self):
    return {
      'title': 'Environment Variables',
      'description': 'List of the environment variables that can be set to alter the behavior of the Infinit storage platform.',
    }

  @route('/documentation/best-practices', name = 'doc_best_practices')
  @view('pages/docs/best_practices.html')
  def root(self):
    return {
      'title': 'Best Practices',
      'description': 'Best practices when administrating an Infinit storage infrastructure.',
    }

  @route('/documentation/comparison/', name = 'doc_comparisons')
  @route('/documentation/comparison/<path:path>', name = 'doc_comparison')
  @view('pages/docs/comparison.html')
  def root(self, path):
    file = resources_path() + '/json/comparisons.json'
    with open(file, encoding = 'utf-8') as json_file:
      json_data = json.load(json_file)

    referer = bottle.request.params.get('from')
    show_comparison = referer == 'faq'

    return {
      'title': json_data[path]['name'] + ' Comparison',
      'description': 'Compare Infinit with the other file storage solutions on the market.',
      'competitor': json_data[path],
      'competitor_name': path,
      'infinit': json_data['infinit'],
      'json': json_data,
      'show_comparison': show_comparison,
    }

  @route('/documentation/key-value-store', name = 'doc_kv_overview')
  @view('pages/docs/kv_overview.html')
  def root(self):
    file = resources_path() + '/scripts/kv/doughnut.json'
    with open(file, encoding = 'utf-8') as json_file:
      json_data = json.load(json_file)

    return {
      'title': 'Key-Value Store',
      'description': 'Infinit provides a distributed decentralized key-value store, with built-in replication and security.',
      'proto': json_data
    }

  @route('/documentation/key-value-store-api', name = 'doc_kv_api')
  @view('pages/docs/kv_api.html')
  def root(self):
    file = resources_path() + '/scripts/kv/doughnut.json'
    with open(file, encoding = 'utf-8') as json_file:
      json_data = json.load(json_file)

    return {
      'title': 'API Key-Value Store',
      'description': 'Check out the API of our decentralized key-value store.',
      'proto': json_data
    }

  @route('/open-source', name = 'opensource')
  @view('pages/opensource.html')
  def root(self):
    file = resources_path() + '/json/opensource.json'
    with open(file, encoding = 'utf-8') as json_file:
      json_data = json.load(json_file)

    return {
      'title': 'Projects',
      'description': 'Check out our open source projects and join a growing community of developers.',
      'projects': json_data['projects']
    }

  @route('/pricing', name = 'pricing')
  @view('pages/pricing.html')
  def root(self):
    return {
      'title': 'Pricing',
      'description': 'Infinit provides a free community version and an entreprise license with additional features.',
    }

  @route('/press', name = 'press')
  @view('pages/press/pr_docker.html')
  def root(self):
    return {
      'title': 'Press Releases',
      'description': 'See all our tech related press releases and download our press kit.',
    }

  @route('/contact', name = 'contact')
  @view('pages/contact.html')
  def root(self):
    return {
      'title': 'Contact Us',
      'description': 'Get in touch with a sales representative of Infinit.',
    }

  @route('/contact', method = 'POST')
  def root(self):
    fields = ['first_name', 'last_name', 'email', 'message']
    if not all(bottle.request.forms.get(field) for field in fields):
      return error(400)
    else:
      response = self.__swu.send(
        email_id = 'tem_XvZ5rnCzWqiTv6NLawEET4',
        recipient = {'address': 'contact@infinit.sh'},
        sender = {
          'reply_to': bottle.request.forms.get('email')
        },
        email_data = {
          f: bottle.request.forms.get(f) for f in ['first_name', 'last_name', 'email', 'message', 'phone', 'company', 'country'] if bottle.request.forms.get(f)
        })
      if response.status_code // 100 != 2:
        return error(503)
      else:
        return {}

  @route('/legal', name = 'legal')
  def root(self):
    redirect("https://www.docker.com/docker-terms-service")

  @route('/slack', name = 'slack', method = 'POST')
  def root(self):
    email = bottle.request.forms.get('email')
    if not email:
      return error(400, reason = 'missing mandatory email')
    else:
      response = self.__swu.send(
        email_id = 'tem_XvZ5rnCzWqiTv6NLawEET4',
        recipient = {'address': 'contact@infinit.sh'},
        sender = {
          'reply_to': bottle.request.forms.get('email')
        },
        email_data = {
          'email': email,
          'message': '%s wants to join the Slack community.' % (email),
        })
      if response.status_code // 100 != 2:
        bottle.response.status = 503
        return {}
      else:
        return {}
    return {}

  @route('/users/confirm_email', name = 'confirm_email', method = 'GET')
  @view('pages/users/confirm_email.html')
  def root(self):
    for field in ['name', 'confirmation_code']:
      if bottle.request.params.get(field) is None:
        return error(400, 'missing mandatory %s' % field)
    email = bottle.request.params.get('email')
    confirmation_code = bottle.request.params.get('confirmation_code')
    name = bottle.request.params.get('name')
    url = '%s/users/%s/confirm_email' % (self.__hub, name)
    if email is not None:
      import urllib.parse
      url += '/%s' % urllib.parse.quote_plus(email)
    import json
    try:
      response = requests.post(
        url = url,
        data = json.dumps({
          'confirmation_code': confirmation_code
        }),
      headers = {'Content-Type': 'application/json'},
      )
      if (response.status_code // 100 != 2 and response.status_code != 410):
        return error(response.status_code,
                     reason = 'server error %s' % response.status_code)
    except requests.exceptions.ConnectionError:
      return error(503)
    errors = []
    try:
      errors = response.json()['errors']
    except Exception:
      pass
    return {
      'title': 'Confirm Email',
      'description': 'Confirm your email and start using Infinit.',
    }

  @route('/css/<path:path>')
  @route('/fonts/<path:path>')
  @route('/images/<path:path>')
  @route('/js/<path:path>')
  @route('/json/<path:path>')
  @route('/scripts/<path:path>')
  def images(self, path):
    d = bottle.request.urlparts.path.split('/')[1]
    return static_file('%s/%s' % (d,  path))

  @route('/robots.txt')
  def file(self):
    return static_file('robots.txt')

  @route('/sitemap.xml')
  def file(self):
    return static_file('sitemap.xml')
