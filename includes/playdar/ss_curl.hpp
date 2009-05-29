/*
    Playdar - music content resolver
    Copyright (C) 2009  Richard Jones
    Copyright (C) 2009  Last.fm Ltd.

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/
#ifndef __CURL_STRAT_H__
#define __CURL_STRAT_H__

#include <boost/asio.hpp>
#include <boost/bind.hpp>
#include <boost/algorithm/string.hpp>
#include <boost/thread/thread.hpp>
#include <boost/thread/mutex.hpp>
#include <boost/thread/condition.hpp>

#include <curl/curl.h>

#include "playdar/streaming_strategy.h"

namespace playdar {

using namespace boost::asio::ip;
/*
    Can stream from anything cURL can.. 
    
    TODO handle failure/slow streams better.
    TODO potential efficiency gain by storing data in lumps of size returned by curl
    TODO make some of the curl options user-configurable (ssl cert checking, auth, timeouts..)
*/
class CurlStreamingStrategy : public StreamingStrategy
{
public:
    CurlStreamingStrategy(std::string url)
        : m_curl(0)
        , m_slist_headers(0)
        , m_url(url)
        , m_thread(0)
        , m_headthread(0)
        , m_abort(false)
        , m_headersFetched( false )
        , m_contentlen( -1 )
    {
        url = boost::to_lower_copy( m_url );
        std::vector<std::string> parts;
        boost::split( parts, url, boost::is_any_of( ":" ));
        if( parts.size() > 1 )
            m_protocol = parts[0];
        
        std::cout << "CTOR ss_curl: " << url << std::endl;
        reset();
    }
    
    /// copy constructor, used by get_instance()
    CurlStreamingStrategy(const CurlStreamingStrategy& other)
        : m_curl(0)
        , m_url(other.url())
        , m_thread(0)
        , m_abort(false)
    {
        reset();
    }

    ~CurlStreamingStrategy()
    { 
        std::cout << "DTOR ss_curl: " << m_url << std::endl;
        if(m_thread)
        {
            m_abort = true; // will cause thread to terminate
            m_thread->join();
            delete m_thread;
        }
        
        if(m_headthread)
        {
            m_headthread->join();
            delete m_headthread;
        }
        
        reset(); 
    }

    /// this returns a shr_ptr to a copy, because it's not threadsafe 
    virtual boost::shared_ptr<StreamingStrategy> get_instance()
    {
        // make a copy:
        return boost::shared_ptr<StreamingStrategy>(new CurlStreamingStrategy(*this));
    }
    
    void set_extra_header(const std::string& header)
    {
        m_slist_headers = curl_slist_append(m_slist_headers, header.c_str());
    }

    std::string mime_type()
    {
        if( m_mimetype.size() )
            return m_mimetype;
        
        if( m_url.size() < 3 )
            return ext2mime( "" );
        
        std::string ext = m_url.substr( m_url.size() - 3 );
        return ext2mime( ext );
    }
    
    int content_length()
    {
        if( !m_headersFetched )
            get_headers();
        
        boost::mutex::scoped_lock lk(m_mut);
        if( !m_headersFetched )
            m_headcond.wait( lk );
        
        if( m_headthread )
        {
            delete m_headthread;
            m_headthread = 0;
        }
        
        return m_contentlen;
    }

    
    std::string debug()
    { 
        std::ostringstream s;
        s << "CurlStreamingStrategy( " << m_url << " )";
        return s.str();
    }
    
    void reset()
    {
        m_connected = false;
        m_writing = false;
        m_bytesreceived = 0;
        m_curl_finished = false;
        m_abort = false;
        m_buffers.clear();
    }

    /// curl callback when data from fetching an url has arrived
    static size_t curl_headfunc( void *vptr, size_t size, size_t nmemb, void *custom )
    {
        CurlStreamingStrategy * inst = ((CurlStreamingStrategy*)custom);
        char * ptr = (char*) vptr;
        size_t len = size * nmemb;
        std::string s( ptr, len );
        boost::to_lower( s );
        std::vector<std::string> v;
        boost::split( v, s, boost::is_any_of( ":" ));
        if( v.size() != 2 )
            return len;
        
        boost::trim( v[0] );
        boost::trim( v[1] );
        if( v[0] == "content-length" )
            try {
                inst->m_contentlen = boost::lexical_cast<int>( v[1] );
            }catch( ... )
            {}
        else if( v[0] == "content-type" )
            inst->m_mimetype = v[1];
        
        return len;
    }
    
    /// curl callback when data from fetching an url has arrived
    static size_t curl_writefunc( void *vptr, size_t size, size_t nmemb, void *custom )
    {
        CurlStreamingStrategy * inst = ((CurlStreamingStrategy*)custom);
        if( !inst->m_connected ) return 1; // someone called reset(), abort transfer

        //starting to read the body of the response
        //so most of the time we can assume there are no more headers;
        inst->m_headersFetched = true;
        inst->m_headcond.notify_all();
        
        size_t len = size * nmemb;
        inst->enqueue(std::string((char*) vptr, len));
        return len;
    }
    
    static int curl_progressfunc(void *clientp/*this instance*/, double dltotal, double dlnow, 
                                 double ultotal, double ulnow)
    {
        //cout << "Curl has downloaded: " << dlnow << " : " << dltotal << endl;
        if( ((CurlStreamingStrategy*)clientp)->m_abort )
        {
            std::cout << "Aborting in-progress download." << std::endl;
            return 1; // non-zero aborts download.
        }
        else
        {
            return 0;
        }
    }
    
    /// run in a thread to do the curl transfer
    void curl_perform()
    {
        std::cout << "doing curl_perform for '" << m_url << "'" << std::endl;
        // this blocks until transfer complete / error:
        m_curlres = curl_easy_perform( m_curl );
        m_curl_finished = true;
        if (m_slist_headers) 
            curl_slist_free_all(m_slist_headers);
        std::cout << "curl_perform done. ret: " << m_curlres << std::endl;
        if (m_curlres) {
            std::cout << "Curl error: " << m_curlerror << std::endl;
        }
        enqueue(std::string());     // end the async_delegate callbacks
        curl_easy_cleanup( m_curl );
        
        m_headersFetched = true;
    }
    
    /// run in a thread to do the curl transfer
    void curl_perform_head()
    {
        std::cout << "doing curl_perform_head for '" << m_url << "'" << std::endl;
        // this blocks until transfer complete / error:
        m_curlres = curl_easy_perform( m_curl );
        m_headersFetched = true;
        std::cout << "curl_perform_head done. ret: " << m_curlres << std::endl;
        if(m_curlres != 0) std::cout << "Curl error: " << m_curlerror << std::endl;
        m_headcond.notify_all();
    }
    
    const std::string url() const { return m_url; }

    typedef boost::function< void(boost::asio::const_buffer)> WriteFunc;

    // virtual
    bool async_delegate(moost::http::reply::WriteFunc writefunc)
    {
        if (!writefunc) {
            // aborted by the moost::http side.
            m_abort = true;
            m_wf = 0;
            return false;
        }

        if (m_abort) {
            m_wf = 0;
            return false;
        }

        m_wf = writefunc;

        if(!m_connected) connect();
        if(!m_connected)
        {
            std::cout << "ERROR: connect failed in httpss." << std::endl;
            if( m_curl ) curl_easy_cleanup( m_curl );
            reset();
            m_wf = 0;
            return false;
        }

        {
            boost::lock_guard<boost::mutex> lock(m_mutex);

            if (m_writing && m_buffers.size()) {
                // previous write has completed:
                m_buffers.pop_front();
                m_writing = false;
            }

            if (!m_writing && m_buffers.size()) {
                // write something new
                m_writing = true;
                m_wf(
                    moost::http::reply::async_payload(
                        boost::asio::const_buffer(m_buffers.front().data(), m_buffers.front().length())));
            }
        }

        bool result =  m_writing || !m_curl_finished;
        if (result == false) {
            m_wf = 0;
        }
        return result;
    }



protected:
    
    void get_headers()
    {
        if( m_headthread )
        {
            m_headthread->join();
            return;
        }
        //Don't rely on head request with http(s) protocol.
        //Instead get headers on connection.
        if( m_protocol == "http" ||
            m_protocol == "https" )
            return connect();
        
        //Try HEAD request
        m_curl = curl_easy_init();
        prep_curl( m_curl );
        curl_easy_setopt( m_curl, CURLOPT_NOBODY, 1 );

        m_headthread = new boost::thread( boost::bind( &CurlStreamingStrategy::curl_perform_head, this ) );
    }

    
    void connect()
    {
        if(m_thread)
        {
            m_thread->join();
        }
        std::cout << debug() << std::endl; 
        reset();
        if(!m_curl)
            m_curl = curl_easy_init();
        
        if(!m_curl)
        {
            std::cout << "Curl init failed" << std::endl;
            throw;
        }
        
        prep_curl( m_curl );
        
        m_connected = true; 
        // do the blocking-fetch in a thread:
        m_thread = new boost::thread( boost::bind( &CurlStreamingStrategy::curl_perform, this ) );
    }
    
    void prep_curl(CURL * handle)
    {
        // for curl options, see:
        // http://curl.netmirror.org/libcurl/c/curl_easy_setopt.html
        curl_easy_setopt( handle, CURLOPT_NOSIGNAL, 1 );
        curl_easy_setopt( handle, CURLOPT_NOBODY, 0 );
        curl_easy_setopt( handle, CURLOPT_CONNECTTIMEOUT, 5 );
        curl_easy_setopt( handle, CURLOPT_SSL_VERIFYPEER, 0 );
        curl_easy_setopt( handle, CURLOPT_FTP_RESPONSE_TIMEOUT, 10 );
        curl_easy_setopt( handle, CURLOPT_URL, m_url.c_str() );
        curl_easy_setopt( handle, CURLOPT_FOLLOWLOCATION, 1 );
        curl_easy_setopt( handle, CURLOPT_MAXREDIRS, 5 );
        curl_easy_setopt( handle, CURLOPT_USERAGENT, "Playdar (libcurl)" );
        curl_easy_setopt( handle, CURLOPT_HTTPHEADER, m_slist_headers );
        curl_easy_setopt( handle, CURLOPT_WRITEFUNCTION, &CurlStreamingStrategy::curl_writefunc );
        curl_easy_setopt( handle, CURLOPT_WRITEDATA, this );
        curl_easy_setopt( handle, CURLOPT_HEADERFUNCTION, &CurlStreamingStrategy::curl_headfunc );
        curl_easy_setopt( handle, CURLOPT_HEADERDATA, this );
        curl_easy_setopt( handle, CURLOPT_ERRORBUFFER, (char*)&m_curlerror );
        // we use the curl progress callbacks to abort transfers mid-download on exit
        curl_easy_setopt( handle, CURLOPT_NOPROGRESS, 0 );
        curl_easy_setopt( handle, CURLOPT_PROGRESSFUNCTION,
                         &CurlStreamingStrategy::curl_progressfunc );
        curl_easy_setopt( handle, CURLOPT_PROGRESSDATA, this );
    }
    
    //FIXME: DUPLICATED from scanner.cpp
    //this should be moved into utils
    std::string ext2mime(std::string ext)
    { 
        if(ext=="mp3") return "audio/mpeg";
        if(ext=="aac") return "audio/mp4";
        if(ext=="mp4") return "audio/mp4";
        if(ext=="m4a") return "audio/mp4"; 
        std::cerr << "Warning, unhandled file extension. Don't know mimetype for " << ext << std::endl;
        //generic:
        return "application/octet-stream";
    }

    CURL *m_curl;
    struct curl_slist * m_slist_headers; // extra headers to be sent
    CURLcode m_curlres;
    bool m_connected;
    std::string m_url;
    std::string m_protocol;
    boost::mutex m_mut;
    boost::condition m_headcond;
    bool m_curl_finished;
    size_t m_bytesreceived;
    char m_curlerror[CURL_ERROR_SIZE];
    boost::thread * m_thread;
    boost::thread * m_headthread;
    bool m_abort;
    bool m_headersFetched;
    
    int m_contentlen;
    std::string m_mimetype;

    /////
    void enqueue(const std::string& s)
    {
        boost::lock_guard<boost::mutex> lock(m_mutex);
        m_buffers.push_back(s);
        if (!m_writing) {
            m_writing = true;
            m_wf(
                moost::http::reply::async_payload(
                    boost::asio::const_buffer(m_buffers.front().data(), m_buffers.front().length())));
        }
    }

    moost::http::reply::WriteFunc m_wf;     // keep a hold of the write func to keep the connection alive.
    boost::mutex m_mutex;
    bool m_writing;
    std::list<std::string> m_buffers;

};

}

#endif
