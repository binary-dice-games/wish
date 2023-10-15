#pragma once

#include <string>
#include <iostream>
#include <streambuf>

namespace bdg {
namespace wish {

class istream : public std::istream {
 private:
  class streambuf : public std::streambuf {
   public:
    streambuf(std::istream& in) : in_(in) {}

   protected:
    int_type underflow() override {
      int_type c = in_.get();
      if (c != traits_type::eof()) {
        char ch = traits_type::to_char_type(c);
        setg(&ch, &ch, &ch + 1);
      }
      return c;
    }

   private:
    std::istream& in_;
  };

 public:
  istream(std::istream& in)
      : std::istream(&buffer_), buffer_(in) {}

 private:
  streambuf buffer_;
};

class ostream : public std::ostream {
 private:
  class streambuf : public std::streambuf {
   public:
    streambuf(std::ostream& out) : out_(out) {}

   protected:
    int_type overflow(int_type c) override {
      if (c != traits_type::eof()) {
        out_.put(c);
      }
      return c;
    }

    int sync() override {
      out_.flush();
      return 0;
    }

   private:
    std::ostream& out_;
  };

 public:
  ostream(std::ostream& out) : std::ostream(&buffer_), buffer_(out) {}

 private:
  streambuf buffer_;
};

} // namespace wish
} // namespace bdg
