//
// Copyright (c) 2024 Mohammad Nejati
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/http_io
//

#include "any_iostream.hpp"

#ifdef _MSC_VER
#include <io.h>
#else
#include <unistd.h>
#endif

namespace
{
bool
is_stdout_tty()
{
#ifdef _MSC_VER
    return _isatty(_fileno(stdout));
#else
    return isatty(fileno(stdout));
#endif
}
} // namespace

any_ostream::any_ostream()
    : stream_{ &std::cout }
    , is_tty_{ ::is_stdout_tty() }
{
}

any_ostream::any_ostream(core::string_view path)
{
    if(path == "-")
    {
        stream_.emplace<std::ostream*>(&std::cout);
    }
    else if(path == "%")
    {
        stream_.emplace<std::ostream*>(&std::cerr);
    }
    else
    {
        auto& f = stream_.emplace<std::ofstream>();
        f.exceptions(std::ofstream::badbit);
        f.open(path);
        if(!f.is_open())
            throw std::runtime_error{ "Couldn't open file" };
    }
}

bool
any_ostream::is_tty() const noexcept
{
    return is_tty_;
}

any_ostream::
operator std::ostream&()
{
    if(auto* s = std::get_if<std::ofstream>(&stream_))
        return *s;
    return *std::get<std::ostream*>(stream_);
}

// -----------------------------------------------------------------------------

any_istream::any_istream(core::string_view path)
{
    if(path == "-")
    {
        stream_.emplace<std::istream*>(&std::cin);
    }
    else
    {
        auto& f = stream_.emplace<std::ifstream>();
        f.exceptions(std::ifstream::badbit);
        f.open(path);
        if(!f.is_open())
            throw std::runtime_error{ "Couldn't open file" };
    }
}

any_istream::
operator std::istream&()
{
    if(auto* s = std::get_if<std::ifstream>(&stream_))
        return *s;
    return *std::get<std::istream*>(stream_);
}
