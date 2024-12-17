//
// Copyright (c) 2024 Mohammad Nejati
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/http_io
//

#include "any_iostream.hpp"
#include "any_stream.hpp"
#include "base64.hpp"
#include "connect.hpp"
#include "cookie.hpp"
#include "file.hpp"
#include "message.hpp"
#include "request.hpp"
#include "utils.hpp"

#include <boost/asio/as_tuple.hpp>
#include <boost/asio/awaitable.hpp>
#include <boost/asio/cancel_after.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/read.hpp>
#include <boost/buffers.hpp>
#include <boost/http_io.hpp>
#include <boost/http_proto.hpp>
#include <boost/scope/scope_fail.hpp>
#include <boost/scope/scope_success.hpp>
#include <boost/url/encode.hpp>
#include <boost/url/parse.hpp>
#include <boost/url/rfc/pchars.hpp>
#include <boost/url/url.hpp>

#include <cstdlib>

namespace http_io  = boost::http_io;
namespace scope    = boost::scope;
using system_error = boost::system::system_error;

#ifdef BOOST_HTTP_PROTO_HAS_ZLIB
inline const bool http_proto_has_zlib = true;
#else
inline const bool http_proto_has_zlib = false;
#endif

void
set_target(
    const operation_config& oc,
    http_proto::request& request,
    const urls::url_view& url)
{
    if(oc.request_target)
    {
        request.set_target(oc.request_target.value());
        return;
    }

    request.set_target(url.encoded_target());
}

struct is_redirect_result
{
    bool is_redirect        = false;
    bool need_method_change = false;
};

is_redirect_result
is_redirect(const operation_config& oc, http_proto::status status) noexcept
{
    // The specifications do not intend for 301 and 302
    // redirects to change the HTTP method, but most
    // user agents do change the method in practice.
    switch(status)
    {
    case http_proto::status::moved_permanently:
        return { true, !oc.post301 };
    case http_proto::status::found:
        return { true, !oc.post302 };
    case http_proto::status::see_other:
        return { true, !oc.post303 };
    case http_proto::status::temporary_redirect:
    case http_proto::status::permanent_redirect:
        return { true, false };
    default:
        return { false, false };
    }
}

bool
is_transient_error(http_proto::status status) noexcept
{
    switch(status)
    {
    case http_proto::status::request_timeout:
    case http_proto::status::too_many_requests:
    case http_proto::status::internal_server_error:
    case http_proto::status::bad_gateway:
    case http_proto::status::service_unavailable:
    case http_proto::status::gateway_timeout:
        return true;
    default:
        return false;
    }
}

bool
can_reuse_connection(
    http_proto::response_view response,
    const urls::url_view& a,
    const urls::url_view& b) noexcept
{
    if(a.encoded_origin() != b.encoded_origin())
        return false;

    if(response.version() != http_proto::version::http_1_1)
        return false;

    if(response.metadata().connection.close)
        return false;

    return true;
}

urls::url
redirect_url(http_proto::response_view response, const urls::url_view& referer)
{
    auto it = response.find(http_proto::field::location);
    if(it != response.end())
    {
        auto rs = urls::parse_uri_reference(it->value);
        if(rs.has_value())
        {
            urls::url url;
            urls::resolve(referer, rs.value(), url);
            return url;
        }
    }
    throw std::runtime_error{ "Bad redirect response" };
}

http_proto::request
create_request(const operation_config& oc, const urls::url_view& url)
{
    using field   = http_proto::field;
    using method  = http_proto::method;
    using version = http_proto::version;

    if(oc.disallow_username_in_url && url.has_userinfo())
        throw std::runtime_error(
            "Credentials was passed in the URL when prohibited");

    auto request = http_proto::request{};

    request.set_method(oc.no_body ? method::head : method::get);

    if(oc.customrequest)
        request.set_method(oc.customrequest.value());

    request.set_version(oc.http10 ? version::http_1_0 : version::http_1_1);
    set_target(oc, request, url);

    request.set(field::host, url.authority().encoded_host_and_port().decode());
    request.set(field::user_agent, oc.useragent.value_or("Boost.Http.Io"));
    request.set(field::accept, "*/*");

    oc.msg.set_headers(request);

    if(oc.resume_from)
    {
        request.set(
            field::range,
            "bytes=" + std::to_string(oc.resume_from.value()) + "-");
    }

    if(oc.range)
        request.set(field::range, "bytes=" + oc.range.value());

    if(!oc.referer.empty())
        request.set(field::referer, oc.referer);

    if(auto creds = oc.userpwd.value_or(url.userinfo()); !creds.empty())
    {
        auto basic_auth = std::string{ "Basic " };
        base64_encode(basic_auth, creds);
        request.set(field::authorization, basic_auth);
    }

    if(oc.encoding && http_proto_has_zlib)
        request.set(field::accept_encoding, "gzip, deflate");

    for(const auto& [_, name, value] : oc.headers)
        request.set(name, value);

    for(const auto& name : oc.omitheaders)
        request.erase(name);

    return request;
}

asio::awaitable<http_proto::status>
perform_request(
    const operation_config& oc,
    std::optional<any_ostream>& header_output,
    std::optional<cookie_jar>& cookie_jar,
    core::string_view exp_cookies,
    ssl::context& ssl_ctx,
    http_proto::context& proto_ctx,
    operation_config::request_info request_info)
{
    using field     = http_proto::field;
    auto executor   = co_await asio::this_coro::executor;
    auto stream     = any_stream{ asio::ip::tcp::socket{ executor } };
    auto parser     = http_proto::response_parser{ proto_ctx };
    auto serializer = http_proto::serializer{ proto_ctx };

    urls::url url = [&]()
    {
        auto rs = normalize_and_parse_url(request_info.url);
        if(rs.has_error())
            throw system_error{ rs.error(), "Failed to parse URL" };
        if(rs.value().host().empty())
            throw std::runtime_error{ "No host part in the URL" };
        return rs.value();
    }();

    {
        auto params = urls::params_view{ oc.query };
        url.encoded_params().append(params.begin(), params.end());
    }

    if(url.path().empty())
        url.set_path("/");

    auto request = create_request(oc, url);

    auto output = [&]()
    {
        auto path = oc.output_dir;

        if(request_info.remotename)
        {
            auto segs = url.encoded_segments();
            if(segs.empty() || segs.back().empty())
                path.append("burl_response");
            else
                path.append(segs.back().begin(), segs.back().end());
        }
        else if(!request_info.output.empty())
        {
            path /= request_info.output;
        }
        else
        {
            return any_ostream{};
        }

        if(oc.create_dirs)
            fs::create_directories(path.parent_path());

        return any_ostream{ path };
    }();

    auto scope_fail = scope::make_scope_fail(
        [&]
        {
            if(oc.rm_partial)
                output.remove_file();
        });

    auto connect_to = [&](any_stream& stream,const urls::url_view& url)
        -> asio::awaitable<void>
    {
        // clean shutdown
        if(oc.proxy.empty())
            co_await stream.async_shutdown(
                asio::cancel_after(ch::milliseconds{ 500 }, asio::as_tuple));

        co_await asio::co_spawn(
            executor,
            connect(oc, ssl_ctx, proto_ctx, stream, url),
            asio::cancel_after(oc.connect_timeout));

        if(oc.recvpersecond)
            stream.write_limit(oc.recvpersecond.value());

        if(oc.sendpersecond)
            stream.write_limit(oc.sendpersecond.value());
    };

    auto stream_headers = [&](http_proto::response_view response)
    {
        if(oc.show_headers)
            output << response.buffer();

        if(header_output.has_value())
            header_output.value() << response.buffer();
    };

    auto set_cookies = [&](const urls::url_view& url, bool trusted)
    {
        auto cookie = cookie_jar ? cookie_jar->make_field(url) : std::string{};

        if(trusted)
            cookie.append(exp_cookies);

        request.erase(field::cookie);

        if(!cookie.empty())
            request.set(field::cookie, cookie);
    };

    auto extract_cookies = [&](const urls::url_view& url)
    {
        if(!cookie_jar)
            return;

        for(auto sv : parser.get().find_all(field::set_cookie))
            cookie_jar->add(url, parse_cookie(sv).value());
    };

    co_await connect_to(stream, url);
    parser.reset();

    auto org_url   = urls::url{ url };
    auto referer   = urls::url{ url };
    auto trusted   = true;
    auto maxredirs = oc.maxredirs;
    auto msg       = oc.msg;
    for(;;)
    {
        set_cookies(url, trusted);
        msg.start_serializer(serializer, request);
        parser.start();

        co_await async_request(stream, serializer, parser, oc.expect100timeout);

        extract_cookies(url);
        stream_headers(parser.get());

        auto [is_redirect, need_method_change] =
            ::is_redirect(oc, parser.get().status());

        if(!is_redirect || !oc.followlocation)
            break;

        if(maxredirs-- == 0)
            throw std::runtime_error{ "Maximum redirects followed" };

        url = redirect_url(parser.get(), referer);

        if(!oc.proto_redir.contains(url.scheme_id()))
            throw std::runtime_error{ "Protocol not supported or disabled" };

        if(can_reuse_connection(parser.get(), referer, url))
        {
            // Discard the body
            // TODO: drop the connection if body is large
            if(request.method() != http_proto::method::head)
            {
                while(!parser.is_complete())
                {
                    parser.consume_body(
                        buffers::buffer_size(parser.pull_body()));
                    co_await http_io::async_read_some(stream, parser);
                }
            }
            else
            {
                parser.reset();
            }
        }
        else
        {
            co_await connect_to(stream, url);
            parser.reset();
        }

        // Change the method according to RFC 9110, Section 15.4.4.
        if(need_method_change && request.method() != http_proto::method::head)
        {
            request.set_method(http_proto::method::get);
            request.erase(field::content_length);
            request.erase(field::content_encoding);
            request.erase(field::content_type);
            request.erase(field::expect);
            msg = {}; // drop the body
        }

        set_target(oc, request, url);

        trusted = (org_url.encoded_origin() == url.encoded_origin()) ||
            oc.unrestricted_auth;

        if(!trusted)
            request.erase(field::authorization);

        if(oc.autoreferer)
        {
            referer.remove_userinfo();
            request.set(field::referer, referer);
        }

        request.set(
            field::host, url.authority().encoded_host_and_port().decode());

        referer = url;
    }

    if(oc.failonerror && parser.get().status_int() >= 400)
        throw std::runtime_error(
            "The requested URL returned error: " +
            std::to_string(parser.get().status_int()));

    // use the server-specified Content-Disposition filename
    if(oc.content_disposition)
    {
        for(auto sv : parser.get().find_all(field::content_disposition))
        {
            auto filepath = extract_filename_form_content_disposition(sv);
            if(filepath.has_value())
            {
                // stripp off the potential path
                auto filename = ::filename(filepath.value());
                if(filename.empty())
                    continue;

                auto path = oc.output_dir;
                path.append(filename.begin(), filename.end());
                output = any_ostream{ path };
                break;
            }
        }
    }

    // stream body
    if(request.method() != http_proto::method::head)
    {
        for(;;)
        {
            for(auto cb : parser.pull_body())
            {
                auto chunk = core::string_view{
                    static_cast<const char*>(cb.data()), cb.size() };

                if(output.is_tty() && chunk.find(char{ 0 }) != chunk.npos)
                {
                    // clang-format off
                    throw std::runtime_error{
                        "Binary output can mess up your terminal.\n"
                        "Use \"--output -\" to tell burl to output it to your terminal anyway, or\n"
                        "consider \"--output <FILE>\" to save to a file." };
                    // clang-format on
                }

                output << chunk;
                parser.consume_body(cb.size());
            }

            if(parser.is_complete())
                break;

            auto [ec, _] = co_await http_io::async_read_some(
                stream, parser, asio::as_tuple);
            if(ec && ec != http_proto::condition::need_more_input)
                throw system_error{ ec };
        }
    }

    // clean shutdown
    if(oc.proxy.empty())
        co_await stream.async_shutdown(
            asio::cancel_after(ch::milliseconds{ 500 }, asio::as_tuple));

    if(oc.failwithbody && parser.get().status_int() >= 400)
        throw std::runtime_error(
            "The requested URL returned error: " +
            std::to_string(parser.get().status_int()));

    co_return parser.get().status();
};

asio::awaitable<void>
retry(
    const operation_config& oc,
    std::function<asio::awaitable<http_proto::status>()> request_task)
{
    auto executor = co_await asio::this_coro::executor;
    auto timer    = asio::steady_timer{ executor };
    auto retries  = oc.req_retry;
    auto max_time = oc.retry_maxtime
        ? ch::steady_clock::now() + oc.retry_maxtime.value()
        : ch::steady_clock::time_point::max();

    auto next_delay = [&, backoff = ch::seconds{ 1 }]() mutable
    {
        if(oc.retry_delay)
            return ch::duration_cast<ch::seconds>(oc.retry_delay.value());
        if(backoff < ch::seconds{ 10 * 60 })
            backoff *= 2;
        return backoff;
    };

    auto can_retry = [&](error_code ec)
    {
        if(retries == 0 || max_time < ch::steady_clock::now())
            return false;

        if(oc.retry_all_errors)
            return true;

        if(!ec)
            return true;

        if(ec == asio::error::operation_aborted)
            return true;

        if(oc.retry_connrefused && ec == asio::error::connection_refused)
            return true;

        return false;
    };

    for(;;)
    {
        try
        {
            auto status = co_await request_task();
            if(!is_transient_error(status) || !can_retry({}))
                break;
            std::cerr << "HTTP error" << std::endl;
        }
        catch(const system_error& e)
        {
            if(!can_retry(e.code()))
                throw;
            std::cerr << e.what() << std::endl;
        }

        auto delay = next_delay();
        std::cerr << "Will retry in " << delay << " seconds. " << retries
                  << " retries left." << std::endl;
        retries--;
        timer.expires_after(delay);
        co_await timer.async_wait();
    }
}

asio::awaitable<void>
co_main(int argc, char* argv[])
{
    auto executor    = co_await asio::this_coro::executor;
    auto oc          = make_operation_config(argc, argv);
    auto proto_ctx   = http_proto::context{};
    auto cookie_jar  = std::optional<::cookie_jar>{};
    auto exp_cookies = std::string{};

    if(oc.nobuffer)
    {
        std::cout.setf(std::ios::unitbuf);
        std::cerr.setf(std::ios::unitbuf);
    }

    // parser service
    http_proto::response_parser::config parser_cfg;
    parser_cfg.body_limit = oc.max_filesize;
    parser_cfg.min_buffer = 1024 * 1024;
    if(http_proto_has_zlib)
    {
        parser_cfg.apply_gzip_decoder    = true;
        parser_cfg.apply_deflate_decoder = true;
        http_proto::zlib::install_service(proto_ctx);
    }
    http_proto::install_parser_service(proto_ctx, parser_cfg);

    auto header_output = [&]() -> std::optional<any_ostream>
    {
        if(!oc.headerfile.empty())
            return any_ostream{ oc.headerfile };
        return std::nullopt;
    }();

    if(oc.enable_cookies)
        cookie_jar.emplace();

    for(auto& s : oc.cookies)
    {
        if(!exp_cookies.empty() && !exp_cookies.ends_with(';'))
            exp_cookies.push_back(';');
        exp_cookies.append(s);
    }

    for(auto& path : oc.cookiefiles)
        any_istream{ path } >> cookie_jar.value();

    if(cookie_jar && oc.cookiesession)
        cookie_jar->clear_session_cookies();

    auto scope_success = scope::make_scope_success(
        [&]()
        {
            if(cookie_jar && !oc.cookiejar.empty())
                any_ostream{ oc.cookiejar } << cookie_jar.value();
        });

    for(const auto& request_info : oc.requests)
    {
        auto request_task = [&]()
        {
            return asio::co_spawn(
                executor,
                perform_request(
                    oc,
                    header_output,
                    cookie_jar,
                    exp_cookies,
                    oc.ssl_ctx,
                    proto_ctx,
                    request_info),
                asio::cancel_after(oc.timeout, asio::use_awaitable));
        };

        co_await retry(oc, request_task);
    }
}

int
main(int argc, char* argv[])
{
    try
    {
        auto ioc = asio::io_context{};
        asio::co_spawn(
            ioc,
            co_main(argc, argv),
            [](std::exception_ptr ep)
            {
                if(ep)
                    std::rethrow_exception(ep);
            });
        ioc.run();
    }
    catch(const std::exception& e)
    {
        std::cerr << e.what() << std::endl;
        return EXIT_FAILURE;
    }
    return EXIT_SUCCESS;
}
