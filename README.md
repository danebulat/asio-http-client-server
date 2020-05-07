## HTTPRequest Class

The chain of asynchronous operations works as followers:

1.  **m_resolver.async_resolve()**

     The application takes the hostname and port number strings and tries to resolve the public IP addresses for that host.

2.  **on_hostname_resolved()**

     Invoked within the callback function to continue the chain of asynchronous operations.

3.  **asio::async_connect()**

     The _async_resolve()_ will pass a _boost::asio::ip::tcp::resolver::iterator object_ to the callback function. This iterator points to one or more endpoints representing the server we wish to connect to. We pass the iterator to _asio::async_connect()_ and connect to the first successful endpoint.

4.  **on_connection_established()**

     Invoked after connecting to the server. We now construct a HTTP GET request message with the following format: _"GET URI HTTP/1.1\r\nHOST: host.tld\r\n\r\n"_.

5.  **asio::async_write()**

     We initiate an asynchronous write operation to send the HTTP request message to the server.

6.  **on_request_set()**

     Invoked after the asynchronous write operation returns. We can assume the server has received our message and can start reading back its response.

7.  **asio::async_read_until()**

     An asynchronous read operation is initiated that will read the server's status line block in its response. We read until the _\r\n_ symbols.

8.  **on_status_line_received()**

     Invoked after the asynchronous read operation returns. The response's status line data will be contained in the _m_response_ object's _asio::streambuf_. We extract the data and cache it in the corresponding _in_response_ data members.

9.  **asio::async_read_until()**

     Another asynchronous read operation is initiated to retrieve the headers block in the server's HTTP response. We read until the _\r\n\r\n_ symbols.

10.  **on_headers_received()**

     Invoked after the asynchronous read operation returns. The response's header data will be contained in the _m_response_ object's _asio::streambuf_. We extract the data and cache it in the corresponding _in_response_ data members.

11.  **asio::async_read()**

     The final asynchronous read operation is initiated to retrieve the body block in the server's HTTP response. We read until the end of the sent response data.
 
12.  **on_response_body_received()**

     Invoked after the final asynchronous read operation returns. The response's body data will be contained in the _m_response_ object's _asio::streambuf_. 

12.  **on_finish()**

     Invoked at the end of the chain or callbacks on both success and error. The passed error code is checked and we respond accordinly. An _asio::error::eof_ error code signifies that the response was read succesfully. Upon succesfully retreiving the full response, we output it with _std::cout_.