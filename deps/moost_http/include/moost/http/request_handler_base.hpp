#ifndef __MOOST_HTTP_REQUEST_HANDLER_BASE_HPP__
#define __MOOST_HTTP_REQUEST_HANDLER_BASE_HPP__

#include "moost/http/reply.hpp"
#include "moost/http/request.hpp"
#include <boost/lexical_cast.hpp>
#include <boost/noncopyable.hpp>
#include <string>
#include <deque>


namespace moost { namespace http {

/// the common handler for all incoming requests.
template<class RequestHandler>
struct request_handler_base
  : private boost::noncopyable
{
  /// handle a request, configure set up headers, pass on to handle_request
  void handle_request_base(const request& req, reply& rep)
  {
    rep.status = reply::ok;
    rep.headers.resize(2);
    rep.headers[0].name = "Content-Length";
    rep.headers[1].name = "Content-Type";
    rep.headers[1].value = "text/plain";

    static_cast< RequestHandler * >(this)->handle_request(req, rep);

    size_t clen = rep.content.size();
    if(rep.streaming())
    {
        clen = rep.streaming_length();
    }
    rep.headers[0].value = boost::lexical_cast<std::string>(clen);
  }

  void handle_request(const request& req, reply& rep)
  {
    // default base implementation does nothing
  }
  

};

}} // moost::http

#endif // __MOOST_HTTP_REQUEST_HANDLER_BASE_HPP__