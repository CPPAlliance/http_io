//
// Copyright (c) 2024 Mohammad Nejati
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/http_io
//

#ifndef BURL_CONNECT_HPP
#define BURL_CONNECT_HPP

#include "any_stream.hpp"
#include "options.hpp"

#include <boost/asio/awaitable.hpp>
#include <boost/asio/ssl/context.hpp>
#include <boost/http_proto/context.hpp>
#include <boost/url/url.hpp>

namespace asio       = boost::asio;
namespace http_proto = boost::http_proto;
namespace ssl        = boost::asio::ssl;
namespace urls       = boost::urls;

asio::awaitable<void>
connect(
    const operation_config& oc,
    ssl::context& ssl_ctx,
    http_proto::context& proto_ctx,
    any_stream& stream,
    urls::url url);

#endif
