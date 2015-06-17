#ifndef INFINIT_MODEL_MODEL_HXX
# define INFINIT_MODEL_MODEL_HXX

namespace infinit
{
  namespace model
  {
    template <typename Block, typename ... Args>
    std::unique_ptr<Block>
    Model::_construct_block(Args&& ... args)
    {
      return std::unique_ptr<Block>(new Block(std::forward<Args>(args)...));
    }
  }
}

#endif
