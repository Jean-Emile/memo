#include <cryptography/KeyPair.hh>
#include <elle/serialization/Serializer.hh>
#include <elle/serialization/json.hh>




using namespace infinit::cryptography;
int main(int argc, char** argv)
{
  if (argc !=2 || argv[1] == std::string("-h") || argv[1] == std::string("--help"))
  {
    std::cerr << "usage: " << argv[0] << " keylength" << std::endl;
    exit(0);
  }
  KeyPair kp = KeyPair::generate(Cryptosystem::rsa, std::stoi(argv[1]));
  elle::serialization::json::SerializerOut output(std::cout, false);
  output.serialize_forward(kp);
}