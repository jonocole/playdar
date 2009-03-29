#ifndef __MOOST_HTTP_CONNECTION_HPP__
#define __MOOST_HTTP_CONNECTION_HPP__

#include "moost/http/reply.hpp"
#include "moost/http/request.hpp"
#include "moost/http/request_handler_base.hpp"
#include "moost/http/request_parser.hpp"
#include <boost/bind.hpp>
#include <boost/asio.hpp>
#include <boost/array.hpp>
#include <boost/noncopyable.hpp>
#include <boost/shared_ptr.hpp>
#include <boost/enable_shared_from_this.hpp>

namespace moost { namespace http {

/// Represents a single connection from a client.
template<class RequestHandler>
class connection
  : public boost::enable_shared_from_this< connection<RequestHandler> >,
    private boost::noncopyable
{
public:
  /// Construct a connection with the given io_service.
  explicit connection(boost::asio::io_service& io_service,
      request_handler_base<RequestHandler>& handler);

  /// Get the socket associated with the connection.
  boost::asio::ip::tcp::socket& socket();

  /// Start the first asynchronous operation for the connection.
  void start();

private:
  /// Handle completion of a read operation.
  void handle_read(const boost::system::error_code& e,
      std::size_t bytes_transferred);

  /// Handle completion of a write operation.
  void handle_write(const boost::system::error_code& e);
  /// Handle completion of headers-sent, then wait on body.
  void handle_write_stream (const boost::system::error_code& e,boost::shared_ptr<StreamingStrategy> ss, char * scratch);
  /// Strand to ensure the connection's handlers are not called concurrently.
  boost::asio::io_service::strand strand_;

  /// Socket for the connection.
  boost::asio::ip::tcp::socket socket_;

  /// The handler used to process the incoming request.
  request_handler_base<RequestHandler>& request_handler_;

  /// Buffer for incoming data.
  boost::array<char, 8192> buffer_;

  /// The incoming request.
  request request_;

  /// The parser for the incoming request.
  request_parser request_parser_;

  /// The reply to be sent back to the client.
  reply reply_;
};

template<class RequestHandler>
connection<RequestHandler>::connection(boost::asio::io_service& io_service,
    request_handler_base<RequestHandler>& handler)
: strand_(io_service),
  socket_(io_service),
  request_handler_(handler)
{
}

template<class RequestHandler>
void connection<RequestHandler>::start()
{
  socket_.async_read_some(boost::asio::buffer(buffer_),
      strand_.wrap(
        boost::bind(&connection<RequestHandler>::handle_read, connection<RequestHandler>::shared_from_this(),
          boost::asio::placeholders::error,
          boost::asio::placeholders::bytes_transferred)));
}

      
template<class RequestHandler>
void connection<RequestHandler>::handle_read(const boost::system::error_code& e,
    std::size_t bytes_transferred)
{
  if (!e)
  {
    boost::tribool result;
    boost::tie(result, boost::tuples::ignore) = request_parser_.parse(
        request_, buffer_.data(), buffer_.data() + bytes_transferred);

    if (result)
    {
      request_handler_.handle_request_base(request_, reply_);
      if(!reply_.streaming()) // normal request
      {
        // send all data, then call the shutdown handler
        boost::asio::async_write(socket_, reply_.to_buffers(),
            strand_.wrap(
                boost::bind(&connection<RequestHandler>::handle_write, connection<RequestHandler>::shared_from_this(),
                boost::asio::placeholders::error)));
      }
      else // streaming enabled, use the streamingstrategy.
      {
        boost::shared_ptr<StreamingStrategy> ss = reply_.get_ss();
        cout << "sending headers.." << endl;
        boost::asio::async_write(socket_, reply_.to_buffers(false),
            /*strand_.wrap(*/
            boost::bind(&connection<RequestHandler>::handle_write_stream,             connection<RequestHandler>::shared_from_this(),
                        boost::asio::placeholders::error, ss, (char*)0))/*)*/;
      }
      
    }
    else if (!result)
    {
      reply_ = reply::stock_reply(reply::bad_request);
      boost::asio::async_write(socket_, reply_.to_buffers(),
          strand_.wrap(
            boost::bind(&connection<RequestHandler>::handle_write, connection<RequestHandler>::shared_from_this(),
              boost::asio::placeholders::error)));
    }
    else
    {
      socket_.async_read_some(boost::asio::buffer(buffer_),
          strand_.wrap(
            boost::bind(&connection<RequestHandler>::handle_read, connection<RequestHandler>::shared_from_this(),
              boost::asio::placeholders::error,
              boost::asio::placeholders::bytes_transferred)));
    }
  }

  // If an error occurs then no new asynchronous operations are started. This
  // means that all shared_ptr references to the connection object will
  // disappear and the object will be destroyed automatically after this
  // handler returns. The connection class's destructor closes the socket.
}

template<class RequestHandler>
boost::asio::ip::tcp::socket& connection<RequestHandler>::socket()
{
  return socket_;
}

template<class RequestHandler>
void connection<RequestHandler>::handle_write(const boost::system::error_code& e)
{
  if (!e)
  {
    // Initiate graceful connection closure.
    boost::system::error_code ignored_ec;
    socket_.shutdown(boost::asio::ip::tcp::socket::shutdown_both, ignored_ec);
  }
}

/// Used when handler sends headers first, then streams body.
template<class RequestHandler>
void connection<RequestHandler>::handle_write_stream 
    (const boost::system::error_code& e, 
     boost::shared_ptr<StreamingStrategy> ss,
     char * scratch)
{
    //cout << "handle_write_stream" << endl;
    if(scratch)
    {
        // free previous buffer
        free(scratch);
    }
    do
    {
        if (!e)
        {
            //cout << "Reading SS...." << endl;
            if(!scratch)
            {
                // scratch is 0 the first time.
                cout << "Initiating ss delivery.." << endl;
            }
        
            if(!ss)
            {
                cout << "StreamingStrat died" << endl;
                break;
            }
            const size_t maxbuf = 4096 * 2;
            char * buf = (char*)malloc(maxbuf);
            int len, total=0;
            try
            {
                len = ss->read_bytes(buf, maxbuf);
                if(len > 0)
                {
                    total += len;
                    //cout << "Sending " << len << " bytes.. " << endl;
                    boost::asio::async_write(socket_, boost::asio::buffer(buf, len),
                       strand_.wrap(
                        boost::bind(&connection<RequestHandler>::handle_write_stream, connection<RequestHandler>::shared_from_this(),
                        boost::asio::placeholders::error, ss, buf)
                       ));
                    return;
                }
                // end of stream..
                cout << "EOS(" << ss->debug() << ")" << endl;
            }
            catch(...)
            {
                cout << "StreamingStrat threw an error. " << endl;
                break;
            }
        }
        else
        {
            cout << "handle_write_stream error for " << ss->debug() 
                 << endl;
            break;
        }
    }while(false);
    
    cout << "Shutting down socket." << endl;
    boost::system::error_code ignored_ec;
        socket_.shutdown(boost::asio::ip::tcp::socket::shutdown_both, ignored_ec);
}


}} // moost::http

#endif // __MOOST_HTTP_CONNECTION_HPP__
