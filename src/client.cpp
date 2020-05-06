/*
HTML Client for sending GET requests to a server.
*/

#include <boost/predef.h> // for identifying the OS

// If the OS is Windows Server 2003 or earlier, enable 
// cancelling of I/O operations.
#ifdef BOOST_OS_WINDOWS
#define __WIN32_WINNT 0x501
#if __WIN32_WINNT <= 0x502

// Disable the usage of the I/O completion port framework as
// it causes problems when cancelling asynchronous operations.
#define BOOST_ASIO_DISABLE_IOCP

// Enable cancelling asynchronous operations
#define BOOST_ENABLE_CANCELIO
#endif
#endif

#include <boost/asio.hpp>
#include <thread>
#include <mutex>
#include <memory>
#include <iostream>

using namespace boost;

// --------------------------------------------------------------------------------
// Application error code definitions and registration with Boost
// --------------------------------------------------------------------------------

namespace http_errors
{   
    // Define error code integers for our custom error category.
    enum http_error_codes
    {
        invalid_response = 1 // when client cannot parse response from server
    };

    // Define custom error_category
    class http_errors_category : public boost::system::error_category
    {
    public:
        // Must override pure virtual functions name() and message()
        const char* name() const BOOST_SYSTEM_NOEXCEPT {
            return "http_errors";
        }

        // Compare the error code integer and output a valid message
        std::string message(int e) const {
            switch (e) {
            case invalid_response:
                return "Server response cannot be parsed.";
                break;
            default:
                return "Unknown error.";
                break;
            }
        }
    };

    // Utility function that instantiates a single static error category and
    // returns it as reference. 
    const boost::system::error_category& get_http_errors_category() {
        static http_errors_category cat;
        return cat;
    }

    // Overload the global make_error_code() free function to support
    // creating error_code objects based on our enum and category.
    boost::system::error_code make_error_code(http_error_codes e) {
        return boost::system::error_code(
            static_cast<int>(e), get_http_errors_category());
    }
}

// Register our error codes enum with the Boost error code system.
namespace boost
{
    namespace system 
    {
        template<>
        struct is_error_code_enum <http_errors::http_error_codes> {
            BOOST_STATIC_CONSTANT(bool, value = true);
        };
    }
}

// --------------------------------------------------------------------------------
// The callback function pointer type declaration
// --------------------------------------------------------------------------------

// Forward delcare HTTP classes.
class HTTPClient;
class HTTPRequest;
class HTTPResponse;

typedef void(*Callback) (const HTTPRequest& request, 
    const HTTPResponse& response, const system::error_code& ec);

// --------------------------------------------------------------------------------
// HTTPResponse class: Represents a HTTP response message sent to the client as a
// response to the request.
// --------------------------------------------------------------------------------

class HTTPResponse 
{
    friend class HTTPRequest;
    
    // Private constructor - only HTTPRequest can invokes it 
    HTTPResponse() : m_response_stream(&m_response_buf)
    {}

public:
    unsigned int get_status_code() const {
        return m_status_code;
    }

    const std::string& get_status_message() const {
        return m_status_message;
    }

    const std::map<std::string, std::string>& get_headers() const {
        return m_headers;
    }

    const std::istream& get_response() const {
        return m_response_stream;
    }

private: // exposed only to HTTPRequest class 
    asio::streambuf& get_response_buf() {
        return m_response_buf;
    }

    void set_status_code(unsigned int status_code) {
        m_status_code = status_code;
    }

    void set_status_message(const std::string& status_message) {
        m_status_message = status_message;
    }

    void add_header(const std::string& name, const std::string& value) {
        m_headers[name] = value;
    }

private:
    unsigned int m_status_code;         // HTTP status code
    std::string m_status_message;       // HTTP status message

    // Response headers
    std::map<std::string, std::string> m_headers;
    asio::streambuf m_response_buf;     // Will contain response data from server 
    std::istream m_response_stream;     // For extracting data in response buffer
};

// --------------------------------------------------------------------------------
// HTTPRequest class: Represents a HTTP GET request that constructs the HTTP
// request message based on information provided by the class users, sends it to
// the server, and then receives and parses the HTTP response message.
// --------------------------------------------------------------------------------

class HTTPRequest
{
    friend class HTTPClient;

    static const unsigned int DEFAULT_PORT = 80;

    // Private constructor - only HTTPClient can invokes it 
    HTTPRequest(asio::io_service& ios, unsigned int id) :
        m_port(DEFAULT_PORT),
        m_id(id),
        m_callback(nullptr),
        m_sock(ios),
        m_resolver(ios),
        m_was_cancelled(false),
        m_ios(ios)
    {}

public:
    // Setters
    void set_host(const std::string& host) { m_host = host; }
    void set_port(unsigned int port) { m_port = port; }
    void set_uri(const std::string& uri) { m_uri = uri; }
    void set_callback(Callback callback) { m_callback = callback; }

    // Getters
    std::string get_host() const { return m_host; }
    unsigned int get_port() const { return m_port; }
    const std::string& get_uri() const { return m_uri; }
    unsigned int get_id() const { return m_id; }

    // Initiates an asynchronous GET request
    void execute() 
    {
        // Ensure that preconditions hold
        assert(m_port > 0);
        assert(m_host.length() > 0);
        assert(m_uri.length() > 0);
        assert(m_callback != nullptr);

        // Prepare the resolve query
        asio::ip::tcp::resolver::query resolver_query(
            m_host, std::to_string(m_port),
            asio::ip::tcp::resolver::query::numeric_service);
        
        // Check if request was cancelled
        std::unique_lock<std::mutex> cancel_lock(m_cancel_mux);

        if (m_was_cancelled) {
            cancel_lock.unlock();
            on_finish(boost::system::error_code(asio::error::operation_aborted));
            return;
        }

        // Resolve the host name (starts asynchronous callback chain)
        m_resolver.async_resolve(resolver_query, 
            [this]
            (const boost::system::error_code& ec, asio::ip::tcp::resolver::iterator iterator)
            {
                on_host_name_resolved(ec, iterator);
            });
    }

    // Cancels the GET request, stops asynchronous callback chain
    void cancel() 
    {
        std::unique_lock<std::mutex> cancel_lock(m_cancel_mux);

        m_was_cancelled = true;

        m_resolver.cancel();

        // Finish all outstanding asynchronous operations immediately on socket.
        // Will pass handlers operation_abort_error error code.
        if (m_sock.is_open()) {
            m_sock.cancel();
        }
    }

private:
    void on_host_name_resolved(const boost::system::error_code& ec, 
        asio::ip::tcp::resolver::iterator iterator)
    {
        // Handle any error codes
        if (ec.value() != 0) {
            on_finish(ec);
            return;
        }

        // Check if request was cancelled
        std::unique_lock<std::mutex> cancel_lock(m_cancel_mux);

        if (m_was_cancelled) {
            cancel_lock.unlock();
            on_finish(boost::system::error_code(asio::error::operation_aborted));
            return;
        }

        // Connect to the first successful endpoint via the iterator
        asio::async_connect(m_sock, iterator, 
            [this](const boost::system::error_code& ec, asio::ip::tcp::resolver::iterator iterator)
            {
                on_connection_established(ec, iterator);
            });
    }

    void on_connection_established(const boost::system::error_code& ec,
        asio::ip::tcp::resolver::iterator iterator)
    {
        // Handle any errors
        if (ec.value() != 0) {
            on_finish(ec);
            return;
        }

        // Compose the request message
        m_request_buf += "GET " + m_uri + " HTTP/1.1\r\n";
        // Add mandatory header
        m_request_buf += "Host: " + m_host + "\r\n";
        // Add final return
        m_request_buf += "\r\n";

        // Check if request was cancelled
        std::unique_lock<std::mutex> cancel_lock(m_cancel_mux);

        if (m_was_cancelled) {
            cancel_lock.unlock();
            on_finish(boost::system::error_code(asio::error::operation_aborted));
            return;
        }

        // Send the request message
        asio::async_write(m_sock, asio::buffer(m_request_buf),
            [this]
            (const boost::system::error_code& ec, std::size_t bytes_transferred) 
            {
                on_request_sent(ec, bytes_transferred);
            });
    }

    void on_request_sent(const boost::system::error_code& ec, std::size_t bytes_transferred)
    {
        // Handle any errors
        if (ec.value() != 0) {
            on_finish(ec);
            return;
        }

        // Let server know the full request is sent by shutting down the socket.
        m_sock.shutdown(asio::ip::tcp::socket::shutdown_send);

        // Check if request was cancelled
        std::unique_lock<std::mutex> cancel_lock(m_cancel_mux);

        if (m_was_cancelled) {
            cancel_lock.unlock();
            on_finish(boost::system::error_code(asio::error::operation_aborted));
            return;
        }

        // Start reading response from server, starting with the status line.
        asio::async_read_until(m_sock, m_response.get_response_buf(), "\r\n",
            [this]
            (const boost::system::error_code& ec, std::size_t bytes_transferred)
            {
                on_status_line_received(ec, bytes_transferred);
            });
    }

    void on_status_line_received(const boost::system::error_code& ec,
        std::size_t bytes_transferred)
    {
        // Handle any errors
        if (ec.value() != 0) {
            on_finish(ec);
            return;
        }

        // Parse the status line
        std::string http_version;
        std::string str_status_code;
        std::string status_message;

        std::istream response_stream(&m_response.get_response_buf());
        response_stream >> http_version; // read until space

        if (http_version != "HTTP/1.1") {
            // Response is incorrect
            on_finish(http_errors::invalid_response);
            return;
        }

        response_stream >> str_status_code; // read until space

        // Convert status code into integer
        unsigned int status_code = 200;
        try {
            status_code = std::stoul(str_status_code); // 302 (found)
        }
        catch (std::logic_error&) {
            // Response is incorrect
            on_finish(http_errors::invalid_response);
            return;
        }

        std::getline(response_stream, status_message, '\r'); // read until '\r'
        response_stream.get(); // Remove '\n' from buffer
        
        m_response.set_status_code(status_code);
        m_response.set_status_message(status_message);

        // Check if request was cancelled
        std::unique_lock<std::mutex> cancel_lock(m_cancel_mux);

        if (m_was_cancelled) {
            cancel_lock.unlock();
            on_finish(boost::system::error_code(asio::error::operation_aborted));
            return;
        }

        // Now read the response headers. According to HTTP protocol, the response
        // headers block ends with "\r\n\r\n" delimiter.
        asio::async_read_until(m_sock, m_response.get_response_buf(), "\r\n\r\n",
            [this]
            (const boost::system::error_code& ec, std::size_t bytes_transferred)
            {
                on_headers_received(ec, bytes_transferred);
            });
    }

    void on_headers_received(const boost::system::error_code& ec,
        std::size_t bytes_transferred)
    {
        // Handle any errors
        if (ec.value() != 0) {
            on_finish(ec);
            return;
        }

        // Parse and store headers
        std::string header, header_name, header_value;
        std::istream response_stream(&m_response.get_response_buf());

        while (true)
        {  
            // Headers example:
            // "Date: Wed, 06 May 2020 01:32:00 GMT"
            // "Server: Apache/2.4.43 (FreeBSD) OpenSSL/1.1.1d-freebsd PHP/7.4.5"
            // "Location: https://distrowatch.com/"
            // "Content-Length: 208"
            // "Content-Type: text/html; charset=iso-8859-1"
            std::getline(response_stream, header, '\r'); // Date header
            response_stream.get(); // remove '\n' symbol

            if (header == "")
                break;
            
            size_t separator_pos = header.find(':');
            if (separator_pos != std::string::npos) {
                // Get header name
                header_name = header.substr(0, separator_pos);
                // Get header value
                if (separator_pos < header.length() - 1)
                    header_value = header.substr(separator_pos + 1);
                else 
                    header_value = "";
                
                m_response.add_header(header_name, header_value);
            }
        }

        // Check if request was cancelled
        std::unique_lock<std::mutex> cancel_lock(m_cancel_mux);

        if (m_was_cancelled) {
            cancel_lock.unlock();
            on_finish(boost::system::error_code(asio::error::operation_aborted));
            return;
        }

        // Now we want to read the response body
        asio::async_read(m_sock, m_response.get_response_buf(),
            [this]
            (const boost::system::error_code& ec, std::size_t bytes_transferred)
            {
                on_response_body_received(ec, bytes_transferred);
            });
    }

    void on_response_body_received(const boost::system::error_code& ec,
        std::size_t bytes_transferred)
    {
        if (ec == asio::error::eof)
            on_finish(boost::system::error_code()); // response body in streambuf
        else
            on_finish(ec);
    }

    // Invokes when request completes (either successfully or not)
    void on_finish(const boost::system::error_code& ec) 
    {
        // Handle error code (can be done in callback)
        if (ec.value() != 0) {
            std::cout << "Error occured.\nError code: " << ec.value() 
                << "\nMessage: " << ec.message() << std::endl;
        }

        // Invoke callback
        m_callback(*this, m_response, ec);

        return;
    }

private:
    // Request parameters
    std::string m_host;
    unsigned int m_port;
    std::string m_uri;

    // Object unique identifier
    unsigned int m_id;

    // Callback to be invoked when request completes
    Callback m_callback;

    // Buffer containing the request line
    std::string m_request_buf;

    // Structure to hold response data received from server
    HTTPResponse m_response;

    // For cancelling mechanism 
    bool m_was_cancelled;
    std::mutex m_cancel_mux;

    asio::ip::tcp::socket m_sock;
    asio::ip::tcp::resolver m_resolver;
    asio::io_service& m_ios;
};

// --------------------------------------------------------------------------------
// HTTPClient: Establishes a threading policy. Spawns and destroys threads in a 
// thread pool. Running the Boost.Asio event loop and delivering asynchronous 
// operation's completion events. Acts as a factory of the HTTPRequest objects.
// --------------------------------------------------------------------------------

class HTTPClient
{
public:
    HTTPClient() {
        m_work.reset(new boost::asio::io_service::work(m_ios));

        m_thread.reset(new std::thread([this](){
            m_ios.run();
        }));
    }

    std::shared_ptr<HTTPRequest> create_request(unsigned int id) {
        return std::shared_ptr<HTTPRequest>(new HTTPRequest(m_ios, id));
    }

    void close() {
        m_work.reset(nullptr); // destroy work object
        m_thread->join(); // wait for I/O thread to exit
    }

private:
    asio::io_service m_ios;
    std::unique_ptr<boost::asio::io_service::work> m_work;
    std::unique_ptr<std::thread> m_thread;  // runs io_service event loop
};

// --------------------------------------------------------------------------------
// The callback implementation: Called when the request completes.
// --------------------------------------------------------------------------------

void handler(const HTTPRequest& request, const HTTPResponse& response, 
    const boost::system::error_code& ec)
{
    if (ec.value() == 0) 
    {
        // If no errors occurred, output the response body.
        std::cout << "Request #" << request.get_id()
            << " has completed. Response: " 
            << response.get_response().rdbuf();  // rdbuf returns pointer to streambuf
    }
    else if (ec == asio::error::operation_aborted) {
        std::cout << "Request #" << request.get_id()
            << " has been cancelled by the user."
            << std::endl;
    }
    else {
        std::cout << "Request #" << request.get_id()
            << "failed. Error code = " << ec.value()
            << ". Error message = " << ec.message()
            << std::endl;
    }

    return;
}

// --------------------------------------------------------------------------------
// main
// --------------------------------------------------------------------------------

int main(int argc, char* argv[])
{
    try 
    {
        // Spawns thread and runs I/O event loop. Is also a HTTPRequest factory. 
        HTTPClient client;

        std::shared_ptr<HTTPRequest> request(client.create_request(1));

        request->set_host("distrowatch.com");
        request->set_uri("/");
        request->set_port(80);
        request->set_callback(handler);

        request->execute();

        // Wait for a period to let request complete.
        std::this_thread::sleep_for(std::chrono::seconds(10));

        // Stop I/O event loop and finish all asynchronous callbacks.
        client.close();
    }
    catch (boost::system::system_error& e)
    {
        std::cerr << "Error occured. Error code = " << e.code()
            << ". Message: " << e.what() << std::endl;
        
        return e.code().value();
    }

    return 0;
}