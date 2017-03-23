#include <elle/test.hh>

#include <elle/err.hh>
#include <elle/Option.hh>

#include <elle/das/Symbol.hh>

#include <boost/optional.hpp>

#include <infinit/model/MissingBlock.hh>
#include <infinit/model/blocks/MutableBlock.hh>
#include <infinit/model/doughnut/ACB.hh>
#include <infinit/model/doughnut/UB.hh>
#include <infinit/overlay/kelips/Kelips.hh>
#include <infinit/overlay/kouncil/Kouncil.hh>
#include <infinit/storage/MissingKey.hh>

#include <infinit/filesystem/filesystem.hh>

#include <infinit/grpc/fs.grpc.pb.h>
#include <infinit/grpc/doughnut.grpc.pb.h>

#include <tests/grpc.grpc.pb.h>

#include <infinit/grpc/grpc.hh>
#include <infinit/grpc/serializer.hh>

#include <grpc++/grpc++.h>

#include "DHT.hh"

ELLE_LOG_COMPONENT("test");


inline
std::string
base_name(std::string const& s)
{
  auto p = s.find_last_of(":");
  if (p == s.npos)
    return s;
  else
    return s.substr(p+1);
}

class Protogen
{
public:
  Protogen() {}
  template<typename T>
  void
  protogen();
  std::unordered_map<std::string, std::string> messages;
};

template<typename T>
struct ProtoTypeName
{
  static std::string value() { return base_name(elle::type_info<T>().name());}
  static void recurse(Protogen& pg) { pg.protogen<T>();}
};

template<typename T>
struct ProtoTypeName<std::vector<T>>
{
  static std::string value() { return "repeated " + ProtoTypeName<T>::value();}
  static void recurse(Protogen& pg) { ProtoTypeName<T>::recurse(pg);}
};

template<typename...T>
struct ProtoTypeName<elle::Option<T...>>
{
  static std::string value() { return "NOTIMPLEMENTED";}
  static void recurse(Protogen& pg) {}
};

#define PROTO_TYPE(ctype, ptype)               \
template<>                                     \
struct ProtoTypeName<ctype>                    \
{                                              \
  static std::string value() { return ptype;}  \
  static void recurse(Protogen& pg) {}         \
}

// FIXME maybe optimize by using 'sint*' protobuf types
PROTO_TYPE(std::string, "string");
PROTO_TYPE(elle::Buffer, "string");
PROTO_TYPE(bool, "bool");
PROTO_TYPE(int64_t, "int64");
PROTO_TYPE(uint64_t, "uint64");
PROTO_TYPE(int32_t, "int64");
PROTO_TYPE(uint32_t, "uint64");
PROTO_TYPE(int16_t, "int32");
PROTO_TYPE(uint16_t, "uint32");
PROTO_TYPE(int8_t, "int32");
PROTO_TYPE(uint8_t, "uint32");
PROTO_TYPE(double, "double");
PROTO_TYPE(float, "double");

template<typename T>
struct ProtoTypeName<boost::optional<T>>
{
  static std::string value()
  {
    return ProtoTypeName<T>::value();
  }
  static void recurse(Protogen& pg) {}
};

template <typename O, typename M = typename elle::das::DefaultModel<O>::type>
struct ProtoHelper
{
  template <typename T>
  struct ProtoProcess
  {
    using type = int;
    static
    int
    value(Protogen& pg, int& fid, std::string& msg)
    {
      using type = typename M::template FieldType<T>::type;
      auto cls = ProtoTypeName<type>::value();
      msg += "  " + cls + " " + T::name() + " = " + std::to_string(++fid) + ";\n";
      ProtoTypeName<type>::recurse(pg);
      return 0;
    }
  };
  static
  void
  protogen(Protogen& pg)
  {
    auto tn = elle::type_info<O>().name();
    pg.messages[tn]; // create it
    int fid = 0;
    std::string msg = "message " + base_name(tn) + " {\n";
    M::Fields::template map<ProtoProcess>::value(pg, fid, msg);
    msg += "}\n";
    pg.messages[tn] = msg;
  }
};

template<typename T>
void
Protogen::protogen()
{
   ProtoHelper<T>::protogen(*this);
}


namespace grpc {
  std::ostream& operator << (std::ostream& o, ::grpc::Status const& s)
  {
    return o << s.error_code() << ": " << s.error_message();
  }
  bool operator == (Status const& a, Status const& b)
  {
    return a.error_code() == b.error_code();
  }
}

class DHTs
{
public:
  template <typename ... Args>
  DHTs(int count)
   : DHTs(count, {})
  {
  }
  template <typename ... Args>
  DHTs(int count,
       boost::optional<elle::cryptography::rsa::KeyPair> kp,
       Args ... args)
    : owner_keys(kp? *kp : elle::cryptography::rsa::keypair::generate(512))
    , dhts()
  {
    pax = true;
    if (count < 0)
    {
      pax = false;
      count *= -1;
    }
    for (int i = 0; i < count; ++i)
    {
      this->dhts.emplace_back(paxos = pax,
                              owner = this->owner_keys,
                              std::forward<Args>(args) ...);
      for (int j = 0; j < i; ++j)
        this->dhts[j].overlay->connect(*this->dhts[i].overlay);
    }
  }

  struct Client
  {
    template<typename... Args>
    Client(std::string const& name, DHT dht, Args...args)
      : dht(std::move(dht))
      , fs(std::make_unique<elle::reactor::filesystem::FileSystem>(
             std::make_unique<infinit::filesystem::FileSystem>(
               name, this->dht.dht, infinit::filesystem::allow_root_creation = true,
               std::forward<Args>(args)...),
             true))
    {}

    DHT dht;
    std::unique_ptr<elle::reactor::filesystem::FileSystem> fs;
  };

  template<typename... Args>
  DHT
  dht(bool new_key,
         boost::optional<elle::cryptography::rsa::KeyPair> kp,
         Args... args)
  {
    auto k = kp ? *kp
    : new_key ? elle::cryptography::rsa::keypair::generate(512)
          : this->owner_keys;
    ELLE_LOG("new client with owner=%f key=%f", this->owner_keys.K(), k.K());
    DHT client(owner = this->owner_keys,
               keys = k,
               storage = nullptr,
               make_consensus = no_cheat_consensus,
               paxos = pax,
               std::forward<Args>(args) ...
               );
    for (auto& dht: this->dhts)
      dht.overlay->connect(*client.overlay);
    return client;
  }
  template<typename... Args>
  Client
  client(bool new_key,
         boost::optional<elle::cryptography::rsa::KeyPair> kp,
         Args... args)
  {
    DHT client = dht(new_key, kp, std::forward<Args>(args)...);
    return Client("volume", std::move(client));
  }

  Client
  client(bool new_key = false)
  {
    return client(new_key, {});
  }

  elle::cryptography::rsa::KeyPair owner_keys;
  std::vector<DHT> dhts;
  bool pax;
};

namespace symbols
{
  ELLE_DAS_SYMBOL(str);
  ELLE_DAS_SYMBOL(i64);
  ELLE_DAS_SYMBOL(ui64);
  ELLE_DAS_SYMBOL(b);
  ELLE_DAS_SYMBOL(ri64);
  ELLE_DAS_SYMBOL(rstr);
  ELLE_DAS_SYMBOL(simple);
  ELLE_DAS_SYMBOL(opt_str);
  ELLE_DAS_SYMBOL(opt_simple);
  ELLE_DAS_SYMBOL(rsimple);
  ELLE_DAS_SYMBOL(siopt);
}

namespace structs
{
  using elle::das::operator << ;
  struct Simple
  {
    bool operator == (const Simple& o) const
    { // DAS save me!
      return str == o.str && i64 == o.i64 && ui64 == o.ui64 && b == o.b && ri64 == o.ri64 && rstr == o.rstr;
    };
    std::string str;
    int64_t i64;
    uint64_t ui64;
    bool b;
    std::vector<int64_t> ri64;
    std::vector<std::string> rstr;
    using Model = elle::das::Model<
    Simple,
    decltype(elle::meta::list(::symbols::str,
                              ::symbols::i64, ::symbols::ui64, ::symbols::b,
                              ::symbols::ri64, ::symbols::rstr))>;
  };
}
ELLE_DAS_SERIALIZE(structs::Simple);

ELLE_TEST_SCHEDULED(serialization)
{
  structs::Simple s{"foo", -42, 42, true,
                    std::vector<int64_t>{0, -12, 42},
                    std::vector<std::string>{"foo", "", "bar"}};
  auto reference = s;
  ::Simple sout;
  {
    infinit::grpc::SerializerOut ser(&sout);
    ser.serialize_forward(s);
  }
  BOOST_CHECK_EQUAL(sout.str(), "foo");
  BOOST_CHECK_EQUAL(sout.i64(), -42);
  BOOST_CHECK_EQUAL(sout.ui64(), 42);
  BOOST_CHECK_EQUAL(sout.b(), true);
  BOOST_CHECK_EQUAL(sout.ri64_size(), 3);
  s = structs::Simple{"", 0, 0, false};
  {
    infinit::grpc::SerializerIn ser(&sout);
    ser.serialize_forward(s);
  }
  BOOST_CHECK_EQUAL(s, reference);
}


namespace structs
{
  struct Complex
  {
    Complex(Simple const& s = Simple{},
            boost::optional<std::string> ost = boost::none,
            boost::optional<Simple> osi = boost::none,
            std::vector<Simple> rs = {},
            elle::Option<std::string, int64_t> sio = (int64_t)0)
    : simple(s)
    , opt_str(ost)
    , opt_simple(osi)
    , rsimple(rs)
    , siopt(sio)
    {}
    bool operator == (const Complex& b) const
    {
      return simple == b.simple
        && opt_str == b.opt_str
        && opt_simple == b.opt_simple
        && rsimple == b.rsimple
        && siopt.is<std::string>() == b.siopt.is<std::string>()
        && (siopt.is<std::string>() ? (siopt.get<std::string>() == b.siopt.get<std::string>())
                                    : (siopt.get<int64_t>() == b.siopt.get<int64_t>()));
    }
    Simple simple;
    boost::optional<std::string> opt_str;
    boost::optional<Simple> opt_simple;
    std::vector<Simple> rsimple;
    elle::Option<std::string, int64_t> siopt;
    using Model = elle::das::Model<
    Complex,
    decltype(elle::meta::list(::symbols::simple,
                              ::symbols::opt_str,
                              ::symbols::opt_simple,
                              ::symbols::rsimple,
                              ::symbols::siopt))>;
  };
}
ELLE_DAS_SERIALIZE(structs::Complex);

ELLE_TEST_SCHEDULED(serialization_complex)
{
  structs::Complex complex {
    structs::Simple{"foo", -42, 42, true},
    std::string("bar"),
    boost::none,
    {},
    std::string("foo")
  };
  ::Complex cplx;
  {
    infinit::grpc::SerializerOut ser(&cplx);
    ser.serialize_forward(complex);
  }
  BOOST_CHECK_EQUAL(cplx.simple().str(), "foo");
  BOOST_CHECK_EQUAL(cplx.simple().i64(), -42);
  BOOST_CHECK_EQUAL(cplx.simple().ui64(), 42);
  BOOST_CHECK_EQUAL(cplx.simple().b(), true);
  BOOST_CHECK_EQUAL(cplx.opt_str(), "bar");
  complex = structs::Complex {
    structs::Simple{"", 0, 0, false},
    std::string()
  };
  {
    infinit::grpc::SerializerIn ser(&cplx);
    ser.serialize_forward(complex);
  }
  BOOST_CHECK_EQUAL(complex.simple.str, "foo");
  BOOST_CHECK_EQUAL(complex.simple.i64, -42);
  BOOST_CHECK_EQUAL(complex.simple.ui64, 42);
  BOOST_CHECK_EQUAL(complex.simple.b, true);
  BOOST_CHECK_EQUAL(complex.opt_str.value_or("UNSET"), "bar");
  // check unset optional<primitive>
  complex.opt_str.reset();
  {
    infinit::grpc::SerializerOut ser(&cplx);
    ser.serialize_forward(complex);
  }
  BOOST_CHECK_EQUAL(cplx.opt_str(), "");
  complex = structs::Complex {
    structs::Simple{"", 0, 0, false},
    std::string("dummy"),
    boost::none,
    {},
    std::string()
  };
  {
    infinit::grpc::SerializerIn ser(&cplx);
    ser.serialize_forward(complex);
  }
  BOOST_CHECK_EQUAL(complex.simple.str, "foo");
  BOOST_CHECK_EQUAL(complex.simple.i64, -42);
  BOOST_CHECK_EQUAL(complex.simple.ui64, 42);
  BOOST_CHECK_EQUAL(complex.simple.b, true);
  // with grpc there is no such concept of optional primitive type, only default value
  BOOST_CHECK_EQUAL(complex.opt_str.value_or("UNSET"), "UNSET");
  // check default value on optional<primitive>
  ELLE_LOG("empty optional<string>");
  complex.opt_str = std::string();
  {
    infinit::grpc::SerializerOut ser(&cplx);
    ser.serialize_forward(complex);
  }
  BOOST_CHECK_EQUAL(cplx.opt_str(), "");
  complex = structs::Complex {
    structs::Simple{"", 0, 0, false},
    std::string("dummy"),
    boost::none,
    {},
    std::string()
  };
  {
    infinit::grpc::SerializerIn ser(&cplx);
    ser.serialize_forward(complex);
  }
  // FAILS BOOST_CHECK_EQUAL(complex.opt_str.value_or("UNSET"), "");

  BOOST_CHECK(!complex.opt_simple);

  complex.opt_simple = structs::Simple{"foo", -12, 12, true};
  {
    infinit::grpc::SerializerOut ser(&cplx);
    ser.serialize_forward(complex);
  }
  BOOST_CHECK_EQUAL(cplx.opt_str(), "");
  complex = structs::Complex {
    structs::Simple{"", 0, 0, false},
    std::string("dummy"),
    boost::none,
    {},
    std::string()
  };
  {
    infinit::grpc::SerializerIn ser(&cplx);
    ser.serialize_forward(complex);
  }
  BOOST_CHECK(complex.opt_simple);
  BOOST_CHECK_EQUAL(complex.opt_simple->str, "foo");

  complex.rsimple.push_back(structs::Simple{"foo", -12, 12, true});
  {
    infinit::grpc::SerializerOut ser(&cplx);
    ser.serialize_forward(complex);
  }
  complex = structs::Complex {
    structs::Simple{"", 0, 0, false},
    std::string("dummy"),
    boost::none,
    {},
    std::string()
  };
  {
    infinit::grpc::SerializerIn ser(&cplx);
    ser.serialize_forward(complex);
  }
  BOOST_CHECK_EQUAL(complex.rsimple.size(), 1);
  BOOST_CHECK_EQUAL(complex.rsimple.front(),
                    (structs::Simple{"foo", -12, 12, true}));
  // Option
  complex.siopt = std::string("foo");
  {
    infinit::grpc::SerializerOut ser(&cplx);
    ser.serialize_forward(complex);
  }
  complex.siopt = (int64_t)42;
  {
    infinit::grpc::SerializerIn ser(&cplx);
    ser.serialize_forward(complex);
  }
  BOOST_CHECK_EQUAL(complex.siopt.get<std::string>(), "foo");
  complex.siopt = (int64_t)42;
    {
    infinit::grpc::SerializerOut ser(&cplx);
    ser.serialize_forward(complex);
  }
  complex.siopt = std::string("foo");
  {
    infinit::grpc::SerializerIn ser(&cplx);
    ser.serialize_forward(complex);
  }
  BOOST_CHECK_EQUAL(complex.siopt.get<int64_t>(), 42);
}

ELLE_TEST_SCHEDULED(protogen)
{
  Protogen p;
  p.protogen<structs::Complex>();
  for (auto const& m: p.messages)
  {
    std::cout << m.second << std::endl;
  }
}

ELLE_TEST_SCHEDULED(filesystem)
{
  DHTs dhts(3);
  auto client = dhts.client();
  infinit::model::Endpoints eps("127.0.0.1", 0);
  auto ep = *eps.begin();
  elle::reactor::Barrier b;
  int listening_port;
  auto t = std::make_unique<elle::reactor::Thread>("grpc",
    [&] {
      b.open();
      infinit::grpc::serve_grpc(*client.dht.dht, *client.fs, ep, &listening_port);
    });
  elle::reactor::wait(b);
  elle::reactor::background([&] {
  auto chan = grpc::CreateChannel(
      elle::sprintf("127.0.0.1:%s", listening_port),
      grpc::InsecureChannelCredentials());
  auto stub = FileSystem::NewStub(chan);
  ::Path path;
  ::FsStatus status;
  { // list /
    path.set_path("/");
    ::DirectoryContent dc;
    grpc::ClientContext context;
    stub->ListDir(&context, path, &dc);
    BOOST_CHECK_EQUAL(dc.status().code(), 0);
    BOOST_CHECK_EQUAL(dc.content_size(), 2);
  }
  // dirs
  path.set_path("/foo");
  { // mkdir
    grpc::ClientContext context;
    stub->MkDir(&context, path, &status);
    BOOST_CHECK_EQUAL(status.code(), 0);
  }
  { // check
    path.set_path("/");
    ::DirectoryContent dc;
    grpc::ClientContext context;
    stub->ListDir(&context, path, &dc);
    BOOST_CHECK_EQUAL(dc.status().code(), 0);
    BOOST_CHECK_EQUAL(dc.content_size(), 3);
    BOOST_CHECK_EQUAL(dc.content(2).name(), "foo");
    BOOST_CHECK_EQUAL(dc.content(2).type(), ENTRY_DIRECTORY);
  }
  { // rmdir
    path.set_path("/foo");
    grpc::ClientContext context;
    stub->RmDir(&context, path, &status);
    BOOST_CHECK_EQUAL(status.code(), 0);
  }
  { // check
    path.set_path("/");
    ::DirectoryContent dc;
    grpc::ClientContext context;
    stub->ListDir(&context, path, &dc);
    BOOST_CHECK_EQUAL(dc.status().code(), 0);
    BOOST_CHECK_EQUAL(dc.content_size(), 2);
  }
  // files
  ::Handle handle;
  ::StatusHandle sh;
  ::HandleBuffer hb;
  ::StatusBuffer sb;
  ::HandleRange hr;
  { // open
    path.set_path("/bar");
    grpc::ClientContext context;
    stub->OpenFile(&context, path, &sh);
    BOOST_CHECK_EQUAL(sh.status().code(), 0);
  }
  { // write
    hb.mutable_handle()->set_handle(sh.handle().handle());
    hb.mutable_buffer()->set_data("barbarbar");
    grpc::ClientContext context;
    stub->Write(&context, hb, &status);
    BOOST_CHECK_EQUAL(status.code(), 0);
  }
  { // close
    grpc::ClientContext context;
    stub->CloseFile(&context, sh.handle(), &status);
    BOOST_CHECK_EQUAL(status.code(), 0);
  }
  { // open
    grpc::ClientContext context;
    stub->OpenFile(&context, path, &sh);
    BOOST_CHECK_EQUAL(sh.status().code(), 0);
  }
  { // read
    hr.mutable_handle()->set_handle(sh.handle().handle());
    hr.mutable_range()->set_size(1000);
    hr.mutable_range()->set_offset(1);
    grpc::ClientContext context;
    stub->Read(&context, hr, &sb);
    BOOST_CHECK_EQUAL(sb.status().code(), 0);
    BOOST_CHECK_EQUAL(sb.buffer().data(), "arbarbar");
  }
  { // open
    path.set_path("/stream");
    grpc::ClientContext context;
    stub->OpenFile(&context, path, &sh);
    BOOST_CHECK_EQUAL(sh.status().code(), 0);
  }
  { // write stream
    grpc::ClientContext context;
    std::unique_ptr<grpc::ClientWriter< ::HandleBuffer> > writer(
      stub->WriteStream(&context, &status));
    hb.mutable_handle()->set_handle(sh.handle().handle());
    for (int i = 0; i< 67; ++i)
    {
      hb.mutable_buffer()->set_offset(16384 * i);
      hb.mutable_buffer()->set_data(std::string(16384, 'a'));
      writer->Write(hb);
    }
    writer->WritesDone();
    writer->Finish();
    BOOST_CHECK_EQUAL(status.code(), 0);
  }
  { // close
    grpc::ClientContext context;
    stub->CloseFile(&context, sh.handle(), &status);
    BOOST_CHECK_EQUAL(status.code(), 0);
  }
  { // open
    grpc::ClientContext context;
    stub->OpenFile(&context, path, &sh);
    BOOST_CHECK_EQUAL(sh.status().code(), 0);
  }
  { // read stream
    hr.mutable_handle()->set_handle(sh.handle().handle());
    hr.mutable_range()->set_size(-1);
    hr.mutable_range()->set_offset(1);
    grpc::ClientContext context;
    std::unique_ptr<grpc::ClientReader< ::StatusBuffer> > reader(
      stub->ReadStream(&context, hr));
    std::string payload;
    while (reader->Read(&sb))
    {
      BOOST_CHECK_EQUAL(sb.status().code(), 0);
      payload += sb.buffer().data();
    }
    reader->Finish();
    BOOST_CHECK_EQUAL(payload.size(), 67 * 16384 - 1);
  }
  });
}

ELLE_TEST_SCHEDULED(doughnut)
{
  elle::Error e("haha bronk");
  try
  {
    throw e;
  }
  catch (...)
  {
    ELLE_LOG("mrouh\n%s", elle::serialization::json::serialize(std::current_exception()));
  }
  DHTs dhts(3);
  auto client = dhts.client();
  auto alice = elle::cryptography::rsa::keypair::generate(512);
  std::unique_ptr<infinit::model::blocks::Block> ubf
    = std::make_unique<infinit::model::doughnut::UB>(
      client.dht.dht.get(), "alice", alice.K(), false);
  std::unique_ptr<infinit::model::blocks::Block> ubr
    = std::make_unique<infinit::model::doughnut::UB>(
      client.dht.dht.get(), "alice", alice.K(), true);
  client.dht.dht->insert(std::move(ubf));
  client.dht.dht->insert(std::move(ubr));

  infinit::model::Endpoints eps("127.0.0.1", 0);
  auto ep = *eps.begin();
  elle::reactor::Barrier b;
  int listening_port = 0;
  auto t = std::make_unique<elle::reactor::Thread>("grpc",
    [&] {
      b.open();
      infinit::grpc::serve_grpc(*client.dht.dht, boost::none, ep, &listening_port);
    });
  elle::reactor::wait(b);
  auto& sched = elle::reactor::scheduler();
  elle::reactor::background([&] {
    auto chan = grpc::CreateChannel(
        elle::sprintf("127.0.0.1:%s", listening_port),
        grpc::InsecureChannelCredentials());
    auto stub = Doughnut::NewStub(chan);
    { // get missing block
      grpc::ClientContext context;
      ::Fetch req;
      ::BlockOrException repl;
      req.set_address(
        std::string((const char*)infinit::model::Address::null.value(), 32));
      ELLE_LOG("call...");
      auto res = stub->fetch(&context, req, &repl);
      ELLE_LOG("...called");
      BOOST_CHECK_EQUAL(res, ::grpc::Status::OK);
      BOOST_CHECK(repl.has_exception_ptr());
      BOOST_CHECK_EQUAL(repl.exception_ptr().exception().type(), "infinit::model::MissingBlock");
    }
    // Basic CHB
    ::Block chb;
    { // make
      grpc::ClientContext context;
      ::IBData data;
      data.set_data("bok");
      stub->make_immutable_block(&context, data, &chb);
      ELLE_LOG("addr: %s", chb.address());
      BOOST_CHECK_EQUAL(chb.address().size(), 32);
      BOOST_CHECK_EQUAL(chb.data(), "bok");
      ELLE_TRACE("addr: %s",
        infinit::model::Address((const uint8_t*)chb.address().data()));
    }
    { // store
      grpc::ClientContext context;
      ::EmptyOrException repl;
      ::Insert insert;
      insert.mutable_block()->CopyFrom(chb);
      ELLE_LOG("insert, type '%s'", insert.block().type());
      stub->insert(&context, insert, &repl);
      ELLE_LOG("...inserted");
      BOOST_CHECK_EQUAL(repl.has_exception_ptr(), false);
    }
    // dht fetch check
    auto a = infinit::model::Address((const uint8_t*)chb.address().data());
    std::string data;
    sched.mt_run<void>("recheck", [&] {
        auto b = client.dht.dht->fetch(a);
        data = b->data().string();
    });
    BOOST_CHECK_EQUAL(data, "bok");
    { // fetch
      grpc::ClientContext context;
      ::BlockOrException abs;
      ::Fetch addr;
      addr.set_address(chb.address());
      stub->fetch(&context, addr, &abs);
      BOOST_CHECK(abs.has_block());
      BOOST_CHECK_EQUAL(abs.block().address(), chb.address());
      BOOST_CHECK_EQUAL(abs.block().data(), "bok");
    }

    // basic OKB
    ::Block okb;
    { // make
      grpc::ClientContext context;
      ::Empty arg;
      stub->make_mutable_block(&context, arg, &okb);
      BOOST_CHECK_EQUAL(okb.address().size(), 32);
      BOOST_CHECK_EQUAL(okb.data(), "");
      ELLE_TRACE("addr: %s",
        infinit::model::Address((const uint8_t*)okb.address().data()));
    }
    okb.set_data_plain("bokbok");
    { // store
      grpc::ClientContext context;
      ::Insert insert;
      ::EmptyOrException repl;
      insert.mutable_block()->CopyFrom(okb);
      ELLE_LOG("insert, type %s", insert.block().type());
      stub->insert(&context, insert, &repl);
      BOOST_CHECK_EQUAL(repl.has_exception_ptr(), false);
    }
    ::BlockOrException abs;
    { // fetch
      grpc::ClientContext context;
      ::Fetch fetch;
      fetch.set_address(okb.address());
      fetch.set_decrypt_data(true);
      stub->fetch(&context, fetch, &abs);
      BOOST_CHECK(abs.has_block());
      BOOST_CHECK_EQUAL(abs.block().address(), okb.address());
      BOOST_CHECK_EQUAL(abs.block().data_plain(), "bokbok");
    }
    // update
    {
      abs.mutable_block()->set_data_plain("mooh");
      grpc::ClientContext context;
      ::EmptyOrException repl;
      ::Update update;
      update.mutable_block()->CopyFrom(abs.block());
      stub->update(&context, update, &repl);
      BOOST_CHECK_EQUAL(repl.has_exception_ptr(), false);
    }
    { // fetch
      ::BlockOrException abs; // use another message to be sure
      grpc::ClientContext context;
      ::Fetch fetch;
      fetch.set_address(okb.address());
      fetch.set_decrypt_data(true);
      stub->fetch(&context, fetch, &abs);
      BOOST_CHECK(abs.has_block());
      BOOST_CHECK_EQUAL(abs.block().address(), okb.address());
      BOOST_CHECK_EQUAL(abs.block().data_plain(), "mooh");
    }
    // update again from same object
    {
      abs.mutable_block()->set_data_plain("merow");
      grpc::ClientContext context;
      ::EmptyOrException repl;
      ::Update update;
      update.mutable_block()->CopyFrom(abs.block());
      stub->update(&context, update, &repl);
      if (repl.has_exception_ptr())
      ELLE_LOG("update: %s", repl.exception_ptr().exception().message());
      BOOST_CHECK_EQUAL(repl.has_exception_ptr(), true);
      // retry update
      {
        grpc::ClientContext context;
        ::Update update;
        BOOST_CHECK(repl.exception_ptr().exception().has_current());
        update.mutable_block()->CopyFrom(repl.exception_ptr().exception().current());
        update.mutable_block()->set_data_plain("merow");
        stub->update(&context, update, &repl);
        BOOST_CHECK_EQUAL(repl.has_exception_ptr(), false);
      }
      { // fetch
        ::BlockOrException tabs;
        grpc::ClientContext context;
        ::Fetch fetch;
        fetch.set_address(okb.address());
        fetch.set_decrypt_data(true);
        stub->fetch(&context, fetch, &tabs);
        BOOST_CHECK(tabs.has_block());
        BOOST_CHECK_EQUAL(tabs.block().data_plain(), "merow");
      }
    }

    // ACB
    /*
    ::Block acb;
    { // make
      grpc::ClientContext context;
      ::Empty arg;
      stub->make_acl_block(&context, arg, &acb);
      BOOST_CHECK_EQUAL(acb.address().size(), 32);
      BOOST_CHECK_EQUAL(acb.data(), "");
      ELLE_TRACE("addr: %s",
        infinit::model::Address((const uint8_t*)acb.address().data()));
    }
    acb.set_data("bokbok");
    { // store
      grpc::ClientContext context;
      ::Block ab;
      ::EmptyOrException repl;
      ab.CopyFrom(acb);
      stub->insert(&context, ab, &repl);
      BOOST_CHECK_EQUAL(repl.has_exception(), false);
    }
    { // fetch
      grpc::ClientContext context;
      ::Address addr;
      addr.set_address(acb.address());
      stub->fetch(&context, addr, &abs);
      BOOST_CHECK(abs.has_block());
      BOOST_CHECK_EQUAL(abs.block().address(), acb.address());
      BOOST_CHECK_EQUAL(abs.block().data(), "bokbok");
    }
    // update
    {
      abs.mutable_block()->set_data("mooh");
      grpc::ClientContext context;
      ::EmptyOrException repl;
      stub->update(&context, abs.block(), &repl);
      BOOST_CHECK_EQUAL(repl.has_exception(), false);
    }
    { // fetch
      ::BlockOrException abs; // use another message to be sure
      grpc::ClientContext context;
      ::Address addr;
      addr.set_address(acb.address());
      stub->fetch(&context, addr, &abs);
      BOOST_CHECK(abs.has_block());
      BOOST_CHECK_EQUAL(abs.block().address(), acb.address());
      BOOST_CHECK_EQUAL(abs.block().data(), "mooh");
    }
    // update again from same object
    {
      abs.mutable_block()->set_data("merow");
      grpc::ClientContext context;
      ::EmptyOrException repl;
      stub->update(&context, abs.block(), &repl);
      BOOST_CHECK_EQUAL(repl.has_exception(), false);
      { // fetch to get current version
        ::BlockOrException tabs;
        grpc::ClientContext context;
        ::Address addr;
        addr.set_address(acb.address());
        stub->fetch(&context, addr, &tabs);
        BOOST_CHECK(tabs.has_block());
        ELLE_TRACE("update version: %s -> %s",
          abs.block().data_version(),
          tabs.block().data_version());
        abs.mutable_block()->set_data_version(tabs.block().data_version());
      }
      // retry update
      {
        grpc::ClientContext context;
        ::EmptyOrException repl;
        stub->update(&context, abs.block(), &repl);
        BOOST_CHECK_EQUAL(repl.has_exception(), false);
      }
      { // fetch
        grpc::ClientContext context;
        ::Address addr;
        addr.set_address(acb.address());
        stub->fetch(&context, addr, &abs);
        BOOST_CHECK(abs.has_block());
        BOOST_CHECK_EQUAL(abs.block().data(), "merow");
      }
    }
    */

    // acls
    /*
    ::KeyOrStatus kohs;
    {
      grpc::ClientContext context;
      ::Bytes name;
      name.set_data("alice");
      stub->UserKey(&context, name, &kohs);
      BOOST_CHECK(kohs.has_key());
    }
    auto* acl = abs.mutable_block()->add_acl();
    acl->set_read(true);
    acl->set_write(true);
    acl->mutable_key_koh()->CopyFrom(kohs.key());
    {
       grpc::ClientContext context;
       ::EmptyOrException status;
       ELLE_TRACE("update with new world perms");
       stub->update(&context, abs.block(), &status);
       BOOST_CHECK_EQUAL(status.has_exception(), false);
    }
    // check read from alice
    sched.mt_run<void>("alice", [&] {
        auto ac = dhts.client(false, alice);
        auto block = ac.dht.dht->fetch(infinit::model::Address((uint8_t*)abs.block().address().data()));
        BOOST_CHECK_EQUAL(block->data(), "merow");
    });
    */
    // username
    /*
    {
      grpc::ClientContext context;
      ::BytesOrStatus bos;
      stub->UserName(&context, kohs.key(), &bos);
      BOOST_CHECK(bos.has_bytes());
      BOOST_CHECK_EQUAL(bos.bytes().data(), "alice");
    }*/

    // NB
    ::Block nb;
    { // make
      grpc::ClientContext context;
      ::NamedBlockKey str;
      str.set_key("uid");
      stub->make_named_block(&context, str, &nb);
    }
    { // insert
      ::Insert insert;
      insert.mutable_block()->CopyFrom(nb);
      insert.mutable_block()->set_data("coin");
      grpc::ClientContext context;
      ::EmptyOrException status;
      stub->insert(&context, insert, &status);
      BOOST_CHECK_EQUAL(status.has_exception_ptr(), false);
    }
    ::Address nba;
    { // ask for address
      grpc::ClientContext context;
      ::NamedBlockKey str;
      str.set_key("uid");
      stub->named_block_address(&context, str, &nba);
    }
    { // fetch
      grpc::ClientContext context;
      ::Fetch fetch;
      fetch.set_address(nba.address());
      ::BlockOrException ab;
      stub->fetch(&context, fetch, &ab);
      BOOST_CHECK(ab.has_block());
      BOOST_CHECK_EQUAL(ab.block().data(), "coin");
    }
    { // dummy address
      grpc::ClientContext context;
      ::NamedBlockKey str;
      str.set_key("invalidid");
      stub->named_block_address(&context, str, &nba);
    }
    { // fetch
      grpc::ClientContext context;
      ::BlockOrException ab;
      ::Fetch fetch;
      fetch.set_address(nba.address());
      stub->fetch(&context, fetch, &ab);
      BOOST_CHECK(ab.has_exception_ptr());
    }
  });
}

ELLE_TEST_SUITE()
{
  auto& master = boost::unit_test::framework::master_test_suite();
  master.add(BOOST_TEST_CASE(serialization), 0, valgrind(10));
  master.add(BOOST_TEST_CASE(serialization_complex), 0, valgrind(10));
  master.add(BOOST_TEST_CASE(doughnut), 0, valgrind(1000));
  master.add(BOOST_TEST_CASE(protogen), 0, valgrind(10));
  master.add(BOOST_TEST_CASE(filesystem), 0, valgrind(60));
  atexit(google::protobuf::ShutdownProtobufLibrary);
}