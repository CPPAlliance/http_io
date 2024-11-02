//
// Copyright (c) 2024 Mohammad Nejati
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/http_io
//

#include <boost/asio.hpp>
#include <boost/asio/ssl.hpp>
#include <boost/buffers.hpp>
#include <boost/http_io.hpp>
#include <boost/http_proto.hpp>
#include <boost/program_options.hpp>
#include <boost/url.hpp>

#include <cstdlib>
#include <iostream>
#include <random>

#if defined(BOOST_ASIO_HAS_CO_AWAIT)

#include <variant>
#include <optional>

namespace asio       = boost::asio;
namespace buffers    = boost::buffers;
namespace core       = boost::core;
namespace http_io    = boost::http_io;
namespace http_proto = boost::http_proto;
namespace po         = boost::program_options;
namespace ssl        = boost::asio::ssl;
namespace urls       = boost::urls;

using error_code   = boost::system::error_code;
using system_error = boost::system::system_error;

#ifdef BOOST_HTTP_PROTO_HAS_ZLIB
inline const bool http_proto_has_zlib = true;
#else
inline const bool http_proto_has_zlib = false;
#endif

core::string_view
mime_type(core::string_view path) noexcept
{
    const auto ext = [&path]
    {
        const auto pos = path.rfind(".");
        if(pos == core::string_view::npos)
            return core::string_view{};
        return path.substr(pos);
    }();

    using ci_equal = urls::grammar::ci_equal;
    if(ci_equal{}(ext, ".gif"))  return "image/gif";
    if(ci_equal{}(ext, ".jpg"))  return "image/jpeg";
    if(ci_equal{}(ext, ".jpeg")) return "image/jpeg";
    if(ci_equal{}(ext, ".png"))  return "image/png";
    if(ci_equal{}(ext, ".svg"))  return "image/svg+xml";
    if(ci_equal{}(ext, ".txt"))  return "text/plain";
    if(ci_equal{}(ext, ".htm"))  return "text/html";
    if(ci_equal{}(ext, ".html")) return "text/html";
    if(ci_equal{}(ext, ".pdf"))  return "application/pdf";
    if(ci_equal{}(ext, ".xml"))  return "application/xml";
    return "application/octet-stream";
}

core::string_view
filename(core::string_view path) noexcept
{
    const auto pos = path.find_last_of("/\\");
    if((pos != std::string_view::npos))
        return path.substr(pos + 1);
    return path;
}

std::uint64_t
filesize(core::string_view path)
{
    http_proto::file file;
    boost::system::error_code ec;

    file.open(
        std::string{ path }.c_str(),
        http_proto::file_mode::scan,
        ec);
    if(ec)
        throw system_error{ ec };

    const auto size = file.size(ec);
    if(ec)
        throw system_error{ ec };

    return size;
}

core::string_view
target(urls::url_view url) noexcept
{
    if(url.encoded_target().empty())
        return "/";

    return url.encoded_target();
}

struct is_redirect_result
{
    bool is_redirect;
    bool need_method_change;
};

is_redirect_result
is_redirect(http_proto::status status) noexcept
{
    // The specifications do not intend for 301 and 302
    // redirects to change the HTTP method, but most
    // user agents do change the method in practice.
    switch(status)
    {
        case http_proto::status::moved_permanently:
        case http_proto::status::found:
        case http_proto::status::see_other:
            return { true, true };
        case http_proto::status::temporary_redirect:
        case http_proto::status::permanent_redirect:
            return { true, false };
        default:
            return { false, false };
    }
}

class any_stream
{
public:
    using executor_type     = asio::any_io_executor;
    using plain_stream_type = asio::ip::tcp::socket;
    using ssl_stream_type   = ssl::stream<plain_stream_type>;

    explicit any_stream(plain_stream_type stream)
        : stream_{ std::move(stream) }
    {
    }

    explicit any_stream(ssl_stream_type stream)
        : stream_{ std::move(stream) }
    {
    }

    executor_type
    get_executor() noexcept
    {
        return std::visit([](auto& s) { return s.get_executor(); }, stream_);
    }

    template<
        typename ConstBufferSequence,
        typename CompletionToken =
            asio::default_completion_token_t<executor_type>>
    auto
    async_write_some(
        const ConstBufferSequence& buffers,
        CompletionToken&& token =
            asio::default_completion_token_t<executor_type>{})
    {
        return boost::asio::async_compose<
            CompletionToken,
            void(error_code, size_t)>(
            [this, buffers, init = false](
                auto&& self,
                error_code ec = {},
                size_t n      = 0) mutable
            {
                if(std::exchange(init, true))
                    return self.complete(ec, n);

                std::visit(
                    [&](auto& s)
                    { s.async_write_some(buffers, std::move(self)); },
                    stream_);
            },
            token,
            get_executor());
    }

    template<
        typename MutableBufferSequence,
        typename CompletionToken =
            asio::default_completion_token_t<executor_type>>
    auto
    async_read_some(
        const MutableBufferSequence& buffers,
        CompletionToken&& token =
            asio::default_completion_token_t<executor_type>{})
    {
        return boost::asio::async_compose<
            CompletionToken,
            void(error_code, size_t)>(
            [this, buffers, init = false](
                auto&& self,
                error_code ec = {},
                size_t n      = 0) mutable
            {
                if(std::exchange(init, true))
                    return self.complete(ec, n);

                std::visit(
                    [&](auto& s)
                    { s.async_read_some(buffers, std::move(self)); },
                    stream_);
            },
            token,
            get_executor());
    }

    template<
        typename CompletionToken =
            asio::default_completion_token_t<executor_type>>
    auto
    async_shutdown(
        CompletionToken&& token =
            asio::default_completion_token_t<executor_type>{})
    {
        return boost::asio::
            async_compose<CompletionToken, void(error_code)>(
                [this, init = false](
                    auto&& self, error_code ec = {}) mutable
                {
                    if(std::exchange(init, true))
                        return self.complete(ec);

                    std::visit(
                        [&](auto& s)
                        {
                            if constexpr(
                                std::is_same_v<decltype(s),ssl_stream_type&>)
                            {
                                s.async_shutdown(std::move(self));
                            }
                            else
                            {
                                s.close(ec);
                                asio::async_immediate(
                                    s.get_executor(),
                                    asio::append(std::move(self), ec));
                            }
                        },
                        stream_);
                },
                token,
                get_executor());
    }

private:
    std::variant<plain_stream_type, ssl_stream_type> stream_;
};

class output_stream
{
    http_proto::file file_;

public:
    output_stream() = default;

    explicit output_stream(core::string_view path)
    {
        error_code ec;
        file_.open(
            std::string{ path }.c_str(),
            http_proto::file_mode::write,
            ec);
        if(ec)
            throw system_error{ ec };
    }

    void
    write(core::string_view sv)
    {
        if(file_.is_open())
        {
            error_code ec;
            file_.write(sv.data(), sv.size(), ec);
            if(ec)
                throw system_error{ ec };
            return;
        }
        std::cout.write(sv.data(), sv.size());
    }
};

class urlencoded_form
{
    std::string body_;

public:
    class source;
    void
    append_text(
        core::string_view name,
        core::string_view value) noexcept
    {
        if(!body_.empty())
            body_ += '&';
        body_ += name;
        if(!value.empty())
            body_ += '=';
        append_encoded(value);
    }

    void
    append_file(core::string_view path)
    {
        http_proto::file file;
        error_code ec;

        file.open(
            std::string{ path }.c_str(),
            http_proto::file_mode::read,
            ec);
        if(ec)
            throw system_error{ ec };

        if(!body_.empty())
            body_ += '&';

        for(;;)
        {
            char buf[64 * 1024];
            const auto read = file.read(buf, sizeof(buf), ec);
            if(ec)
                throw system_error{ ec };
            if(read == 0)
                break;
            append_encoded({ buf, read });
        }
    }

    core::string_view
    content_type() const noexcept
    {
        return "application/x-www-form-urlencoded";
    }

    std::size_t
    content_length() const noexcept
    {
        return body_.size();
    }

    buffers::const_buffer
    body() const noexcept
    {
        return { body_.data(), body_.size() };
    }

private:
    void
    append_encoded(core::string_view sv)
    {
        urls::encoding_opts opt;
        opt.space_as_plus = true;
        urls::encode(
            sv,
            urls::pchars,
            opt,
            urls::string_token::append_to(body_));
    }
};

class multipart_form
{
    struct part_t
    {
        core::string_view name;
        core::string_view value_or_path;
        core::string_view content_type;
        std::optional<std::uint64_t> file_size;
    };

    // storage_ containts boundary with extra "--" prefix and postfix.
    // This reduces the number of steps needed during serialization.
    std::array<char, 2 + 46 + 2> storage_{ generate_boundary() };
    std::vector<part_t> parts_;

    static constexpr core::string_view content_disposition_ =
        "\r\nContent-Disposition: form-data; name=\"";
    static constexpr core::string_view filename_ =
        "; filename=\"";
    static constexpr core::string_view content_type_ =
        "\r\nContent-Type: ";

public:
    class source;

    void
    append_text(
        core::string_view name,
        core::string_view value,
        core::string_view content_type)
    {
        parts_.emplace_back(name, value, content_type );
    }

    void
    append_file(
        core::string_view name,
        core::string_view path,
        core::string_view content_type)
    {
        // store size because file may change on disk between
        // call to content_length and serialization.
        parts_.emplace_back(
            name, path, content_type, filesize(path));
    }

    std::string
    content_type() const noexcept
    {
        std::string res = "multipart/form-data; boundary=";
        // append boundary
        res.append(storage_.begin() + 2, storage_.end() - 2);
        return res;
    }

    std::uint64_t
    content_length() const noexcept
    {
        auto rs = std::uint64_t{};
        for(const auto& part : parts_)
        {
            rs += storage_.size() - 2; // --boundary
            rs += content_disposition_.size();
            rs += part.name.size();
            rs += 1; // closing double quote

            if(!part.content_type.empty())
            {
                rs += content_type_.size();
                rs += part.content_type.size();
            }

            if(part.file_size.has_value()) // file
            {
                rs += filename_.size();
                rs += filename(part.value_or_path).size();
                rs += 1; // closing double quote
                rs += part.file_size.value();
            }
            else // text
            {
                rs += part.value_or_path.size();
            }

            rs += 4; // <CRLF><CRLF> after header
            rs += 2; // <CRLF> after content
        }
        rs += storage_.size(); // --boundary--
        return rs;
    }

private:
    static
    decltype(storage_)
    generate_boundary()
    {
        decltype(storage_) rs;
        constexpr static char chars[] =
            "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz";
        static std::random_device rd;
        std::uniform_int_distribution<int> dist{ 0, sizeof(chars) - 2 };
        std::fill(rs.begin(), rs.end(), '-');
        std::generate(
            rs.begin() + 2 + 24,
            rs.end() - 2,
            [&] {  return chars[dist(rd)]; });
        return rs;
    }
};

class multipart_form::source
    : public http_proto::source
{
    const multipart_form* form_;
    std::vector<part_t>::const_iterator it_{ form_->parts_.begin() };
    int step_           = 0;
    std::uint64_t skip_ = 0;

public:
    explicit source(const multipart_form* form) noexcept
        : form_{ form }
    {
    }

    results
    on_read(buffers::mutable_buffer mb) override
    {
        auto rs = results{};

        auto copy = [&](core::string_view sv)
        {
            auto copied = buffers::buffer_copy(
                mb,
                buffers::sans_prefix(
                    buffers::const_buffer{ sv.data(), sv.size() },
                    static_cast<std::size_t>(skip_)));

            mb = buffers::sans_prefix(mb, copied);
            rs.bytes += copied;
            skip_    += copied;

            if(skip_ != sv.size())
                return false;

            skip_ = 0;
            return true;
        };

        auto read = [&](core::string_view path, uint64_t size)
        {
            http_proto::file file;

            file.open(
                std::string{ path }.c_str(),
                http_proto::file_mode::read,
                rs.ec);
            if(rs.ec)
                return false;

            file.seek(skip_, rs.ec);
            if(rs.ec)
                return false;

            auto read = file.read(
                mb.data(),
                (std::min)(static_cast<
                    std::uint64_t>(mb.size()), size),
                rs.ec);
            if(rs.ec)
                return false;

            mb = buffers::sans_prefix(mb, read);
            rs.bytes += read;
            skip_    += read;

            if(skip_ != size)
                return false;

            skip_ = 0;
            return true;
        };

        while(it_ != form_->parts_.end())
        {
            switch(step_)
            {
                case 0:
                    // --boundary
                    if(!copy({ form_->storage_.data(),
                        form_->storage_.size() - 2 })) return rs;
                    ++step_;
                case 1:
                    if(!copy(content_disposition_)) return rs;
                    ++step_;
                case 2:
                    if(!copy(it_->name)) return rs;
                    ++step_;
                case 3:
                    if(!copy("\"")) return rs;
                    ++step_;
                case 4:
                    if(!it_->file_size.has_value())
                        goto content_type;
                    if(!copy(filename_)) return rs;
                    ++step_;
                case 5:
                    if(!copy(filename(it_->value_or_path))) return rs;
                    ++step_;
                case 6:
                    if(!copy("\"")) return rs;
                    ++step_;
                case 7:
                content_type:
                    if(it_->content_type.empty())
                        goto end_of_header;
                    if(!copy(content_type_)) return rs;
                    ++step_;
                case 8:
                    if(!copy(it_->content_type)) return rs;
                    ++step_;
                case 9:
                end_of_header:
                    if(!copy("\r\n\r\n")) return rs;
                    ++step_;
                case 10:
                    if(it_->file_size)
                    {
                        if(!read(
                            it_->value_or_path,
                            it_->file_size.value())) return rs;
                    }
                    else
                    {
                        if(!copy(it_->value_or_path)) return rs;
                    }
                    ++step_;
                case 11:
                    if(!copy("\r\n"))
                        return rs;
                    step_ = 0;
                    ++it_;
            }
        }

        // --boundary--
        if(!copy({ form_->storage_.data(),
            form_->storage_.size() })) return rs;

        rs.finished = true;
        return rs;
    };
};

class message
{
    std::variant<
        std::monostate,
        urlencoded_form,
        multipart_form> body_;
public:
    message() = default;

    message(urlencoded_form&& form)
        : body_{ std::move(form) }
    {
    }

    message(multipart_form&& form)
        : body_{ std::move(form) }
    {
    }

    void
    set_headers(http_proto::request& req) const
    {
        std::visit(
        [&](auto& f)
        {
            if constexpr(!std::is_same_v<
                decltype(f), const std::monostate&>)
            {
                req.set_method(http_proto::method::post);
                req.set_content_length(f.content_length());
                req.set(
                    http_proto::field::content_type,
                    f.content_type());
            }
        },
        body_);
    }

    void
    start_serializer(
        http_proto::serializer& ser,
        http_proto::request& req) const
    {
        std::visit(
        [&](auto& f)
        {
            if constexpr(std::is_same_v<
                decltype(f), const multipart_form&>)
            {
                ser.start<
                    multipart_form::source>(req, &f);
            }
            else if constexpr(std::is_same_v<
                decltype(f), const urlencoded_form&>)
            {
                ser.start(req, f.body());
            }
            else
            {
                ser.start(req);
            }
        },
        body_);
    }
};


asio::awaitable<any_stream>
connect(ssl::context& ssl_ctx, urls::url_view url)
{
    auto executor = co_await asio::this_coro::executor;
    auto resolver = asio::ip::tcp::resolver{ executor };
    auto service  = url.has_port() ? url.port() : url.scheme();
    auto rresults = co_await resolver.async_resolve(url.host(), service);

    if(url.scheme() == "https")
    {
        auto stream = ssl::stream<asio::ip::tcp::socket>{ executor, ssl_ctx };
        co_await asio::async_connect(stream.lowest_layer(), rresults);

        if(auto host_s = std::string{ url.host() };
           !SSL_set_tlsext_host_name(stream.native_handle(), host_s.c_str()))
        {
            throw system_error{ static_cast<int>(::ERR_get_error()),
                asio::error::get_ssl_category() };
        }

        co_await stream.async_handshake(ssl::stream_base::client);
        co_return stream;
    }

    auto stream = asio::ip::tcp::socket{ executor };
    co_await asio::async_connect(stream, rresults);
    co_return stream;
}

http_proto::request
create_request(
    const po::variables_map& vm,
    const message& msg,
    urls::url_view url)
{
    using http_proto::field;
    using http_proto::method;
    using http_proto::version;

    auto request = http_proto::request{};

    request.set_method(vm.count("head") ? method::head : method::get);

    if(vm.count("request"))
        request.set_method(vm.at("request").as<std::string>());

    request.set_version(
        vm.count("http1.0") ? version::http_1_0 : version::http_1_1);

    request.set_target(target(url));
    request.set(field::accept, "*/*");
    request.set(field::host, url.host());

    msg.set_headers(request);

    if(vm.count("continue-at"))
    {
        auto value = "bytes=" +
            std::to_string(vm.at("continue-at").as<std::uint64_t>()) + "-";
        request.set(field::range, value);
    }

    if(vm.count("range"))
        request.set(field::range, "bytes=" + vm.at("range").as<std::string>());

    if(vm.count("user-agent"))
    {
        request.set(field::user_agent, vm.at("user-agent").as<std::string>());
    }
    else
    {
        request.set(field::user_agent, "Boost.Http.Io");
    }

    if(vm.count("referer"))
        request.set(field::referer, vm.at("referer").as<std::string>());

    if(vm.count("user"))
    {
        // TODO: use base64 encoding for basic authentication
        request.set(field::authorization, vm.at("user").as<std::string>());
    }

    if(vm.count("compressed") && http_proto_has_zlib)
        request.set(field::accept_encoding, "gzip, deflate");

    // Set user provided headers
    if(vm.count("header"))
    {
        for(auto& header : vm.at("header").as<std::vector<std::string>>())
        {
            if(auto pos = header.find(':'); pos != std::string::npos)
                request.set(header.substr(0, pos), header.substr(pos + 1));
        }
    }

    return request;
}

asio::awaitable<void>
request(
    const po::variables_map& vm,
    output_stream& output,
    message& msg,
    ssl::context& ssl_ctx,
    http_proto::context& http_proto_ctx,
    http_proto::request request,
    urls::url_view url)
{
    auto stream     = co_await connect(ssl_ctx, url);
    auto parser     = http_proto::response_parser{ http_proto_ctx };
    auto serializer = http_proto::serializer{ http_proto_ctx };

    msg.start_serializer(serializer, request);
    co_await http_io::async_write(stream, serializer);

    parser.reset();
    parser.start();
    co_await http_io::async_read_header(stream, parser);

    // handle redirects
    auto referer_url = urls::url{ url };
    for(;;)
    {
        auto [is_redirect, need_method_change] =
            ::is_redirect(parser.get().status());

        if(!is_redirect || !vm.count("location"))
            break;

        auto response = parser.get();
        if(auto it = response.find(http_proto::field::location);
           it != response.end())
        {
            auto redirect_url = urls::parse_uri(it->value).value();

            // TODO: reuse the established connection when possible
            co_await stream.async_shutdown(asio::as_tuple);
            stream = co_await connect(ssl_ctx, redirect_url);

            // Change the method according to RFC 9110, Section 15.4.4.
            if(need_method_change && !vm.count("head"))
            {
                request.set_method(http_proto::method::get);
                request.set_content_length(0);
                request.erase(http_proto::field::content_type);
                msg = {}; // drop the body
            }
            request.set_target(target(redirect_url));
            request.set(http_proto::field::host, redirect_url.host());
            request.set(http_proto::field::referer, referer_url);

            referer_url = redirect_url;

            serializer.reset();
            msg.start_serializer(serializer, request);
            co_await http_io::async_write(stream, serializer);

            parser.reset();
            parser.start();
            co_await http_io::async_read_header(stream, parser);
        }
        else
        {
            throw std::runtime_error{ "Bad redirect response" };
        }
    }

    // stream headers
    if(vm.count("head") || vm.count("show-headers"))
        output.write(parser.get().buffer());

    // stream body
    if(request.method() != http_proto::method::head)
    {
        for(;;)
        {
            for(auto cb : parser.pull_body())
            {
                output.write(
                    { static_cast<const char*>(cb.data()), cb.size() });
                parser.consume_body(cb.size());
            }

            if(parser.is_complete())
                break;

            auto [ec, _] =
                co_await http_io::async_read_some(stream, parser, asio::as_tuple);
            if(ec && ec != http_proto::condition::need_more_input)
                throw system_error{ ec };
        }
    }

    // clean shutdown
    auto [ec] = co_await stream.async_shutdown(asio::as_tuple);
    if(ec && ec != ssl::error::stream_truncated)
        throw system_error{ ec };
};

int
main(int argc, char* argv[])
{
    int co_main(int argc, char* argv[]);
    //return co_main(argc, argv);
    try
    {
        auto odesc = po::options_description{"Options"};
        odesc.add_options()
            ("compressed", "Request compressed response")
            ("continue-at,C",
                po::value<std::uint64_t>()->value_name("<offset>"),
                "Resume transfer offset")
            ("data,d",
                po::value<std::vector<std::string>>()->value_name("<data>"),
                "HTTP POST data")
            ("form,F",
                po::value<std::vector<std::string>>()->value_name("<name=content>"),
                "Specify multipart MIME data")
            ("head,I", "Show document info only")
            ("header,H",
                po::value<std::vector<std::string>>()->value_name("<header>"),
                "Pass custom header(s) to server")
            ("help,h", "produce help message")
            ("http1.0", "Use HTTP 1.0")
            ("location,L", "Follow redirects")
            ("output,o",
                po::value<std::string>()->value_name("<file>"),
                "Write to file instead of stdout")
            ("range,r",
                po::value<std::string>()->value_name("<range>"),
                "Retrieve only the bytes within range")
            ("referer,e",
                po::value<std::string>()->value_name("<url>"),
                "Referer URL")
            ("request,X",
                po::value<std::string>()->value_name("<method>"),
                "Specify request method to use")
            ("show-headers,i", "Show response headers in the output")
            ("url",
                po::value<std::string>()->value_name("<url>"),
                "URL to work with")
            ("user,u",
                po::value<std::string>()->value_name("<user:password>"),
                "Server user and password")
            ("user-agent,A",
                po::value<std::string>()->value_name("<name>"),
                "Send User-Agent <name> to server");

        auto podesc = po::positional_options_description{};
        podesc.add("url", 1);

        po::variables_map vm;
        po::store(
            po::command_line_parser{ argc, argv }
                .options(odesc)
                .positional(podesc)
                .run(),
            vm);
        po::notify(vm);

        if(vm.count("help") || !vm.count("url"))
        {
            std::cerr
                << "Usage: burl [options...] <url>\n"
                << "Example:\n"
                << "    burl https://www.example.com\n"
                << "    burl -L http://httpstat.us/301\n"
                << "    burl https://httpbin.org/post -F name=Shadi -F img=@./avatar.jpeg\n"
                << odesc;
            return EXIT_FAILURE;
        }

        auto url = urls::parse_uri(vm.at("url").as<std::string>());
        if(url.has_error())
        {
            std::cerr
                << "Failed to parse URL\n"
                << "Error: " << url.error().what() << std::endl;
            return EXIT_FAILURE;
        }

        auto ioc            = asio::io_context{};
        auto ssl_ctx        = ssl::context{ ssl::context::tlsv12_client };
        auto http_proto_ctx = http_proto::context{};

        ssl_ctx.set_verify_mode(ssl::verify_none);

        {
            http_proto::response_parser::config cfg;
            cfg.body_limit = std::numeric_limits<std::size_t>::max();
            cfg.min_buffer = 1024 * 1024;
            if(http_proto_has_zlib)
            {
                cfg.apply_gzip_decoder    = true;
                cfg.apply_deflate_decoder = true;
                http_proto::zlib::install_service(http_proto_ctx);
            }
            http_proto::install_parser_service(http_proto_ctx, cfg);
        }

        auto output = [&]
        {
            if(vm.count("output"))
                return output_stream{ vm.at("output").as<std::string>() };
            return output_stream{};
        }();

        auto msg = message{};

        if(vm.count("form") && vm.count("data"))
            throw std::runtime_error{
                "You can only select one HTTP request method"};

        if(vm.count("form"))
        {
            auto form = multipart_form{};
            for(auto& data : vm.at("form").as<std::vector<std::string>>())
            {
                if(auto pos = data.find('='); pos != std::string::npos)
                {
                    auto name  = core::string_view{ data }.substr(0, pos);
                    auto value = core::string_view{ data }.substr(pos + 1);
                    if(!value.empty() && value[0] == '@')
                    {
                        form.append_file(
                            name,
                            value.substr(1),
                            mime_type(value.substr(1)));
                    }
                    else
                    {
                        form.append_text(name, value, "");
                    }
                }
                else
                {
                    throw std::runtime_error{
                        "Illegally formatted input field"};
                }
            }
            msg = std::move(form);
        }

        if(vm.count("data"))
        {
            auto form = urlencoded_form{};
            for(auto& data : vm.at("data").as<std::vector<std::string>>())
            {
                if(!data.empty() && data[0] == '@')
                {
                    form.append_file(data.substr(1));
                }
                else
                {
                    if(auto pos = data.find('=');
                        pos != std::string::npos)
                    {
                        form.append_text(
                            data.substr(0, pos),
                            data.substr(pos + 1));
                    }
                    else
                    {
                        form.append_text(data, "");
                    }
                }
            }
            msg = std::move(form);
        }

        asio::co_spawn(
            ioc,
            request(
                vm,
                output,
                msg,
                ssl_ctx,
                http_proto_ctx,
                create_request(vm, msg, url.value()),
                url.value()),
            [](std::exception_ptr ep)
            {
                if(ep)
                    std::rethrow_exception(ep);
            });

        ioc.run();
    }
    catch(std::exception const& e)
    {
        std::cerr << "Error: " << e.what() << std::endl;
        return EXIT_FAILURE;
    }
    return EXIT_SUCCESS;
}

#else

int
main(int, char*[])
{
    std::cerr << "Coroutine examples require C++20" << std::endl;
    return EXIT_FAILURE;
}

#endif
