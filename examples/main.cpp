#include "src/wish.hpp"

using namespace bdg::wish;

int main() {
  ostream out(std::cout);
  out << "Hello world" << std::endl;


  std::string line;
  istream in(std::cin);
  std::getline(in, line);
  out << "Name: " << line << std::endl;

  return 0;
}
