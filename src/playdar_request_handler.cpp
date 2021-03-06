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
#include <iostream>
#include <fstream>
#include <sstream>

#include <boost/foreach.hpp>
#include <boost/algorithm/string.hpp>
#include <boost/thread.hpp>
#include <boost/shared_ptr.hpp>
#include <boost/foreach.hpp>
#include <boost/tokenizer.hpp>

#include <moost/http.hpp>

#include "json_spirit/json_spirit.h"
#include "playdar/application.h"
#include "playdar/playdar_request_handler.h"
#include "playdar/playdar_request.h"
#include "playdar/playdar_response.h"
#include "playdar/resolver.h"
#include "playdar/track_rq_builder.hpp"
#include "playdar/pluginadaptor.h"
#include "playdar/utils/urlencoding.hpp"
#include "playdar/utils/htmlentities.hpp"
#include "playdar/CometSession.hpp"
#include "playdar/HttpAsyncAdaptor.hpp"

using namespace std;

namespace playdar {

using namespace utils;

void 
playdar_request_handler::init(MyApplication * app)
{
    m_disableAuth = app->conf()->get<bool>( "disableauth", false );
    cout << "HTTP handler online." << endl;
    m_pauth = new playdar::auth(app->conf()->get<string>( "authdb", "" ));
    m_app = app;
    // built-in handlers:
    m_urlHandlers[ "" ] = boost::bind( &playdar_request_handler::handle_root, this, _1, _2 );
    m_urlHandlers[ "crossdomain.xml" ] = boost::bind( &playdar_request_handler::handle_crossdomain, this, _1, _2 );
    m_urlHandlers[ "auth_1" ] = boost::bind( &playdar_request_handler::handle_auth1, this, _1, _2 );
    m_urlHandlers[ "auth_2" ] = boost::bind( &playdar_request_handler::handle_auth2, this, _1, _2 );
    m_urlHandlers[ "shutdown" ] = boost::bind( &playdar_request_handler::handle_shutdown, this, _1, _2 );
    m_urlHandlers[ "settings" ] = boost::bind( &playdar_request_handler::handle_settings, this, _1, _2 );
    m_urlHandlers[ "queries" ] = boost::bind( &playdar_request_handler::handle_queries, this, _1, _2 );
    m_urlHandlers[ "static" ] = boost::bind( &playdar_request_handler::serve_static_file, this, _1, _2 );
    m_urlHandlers[ "sid" ] = boost::bind( &playdar_request_handler::handle_sid, this, _1, _2 );
    m_urlHandlers[ "comet" ] = boost::bind( &playdar_request_handler::handle_comet, this, _1, _2 );
    
    //Local Collection / Main API plugin callbacks:
    m_urlHandlers[ "quickplay" ] = boost::bind( &playdar_request_handler::handle_quickplay, this, _1, _2 );
    
    // handlers provided by plugins TODO ask plugin if/what they actually handle anything?
    BOOST_FOREACH( const pa_ptr pap, m_app->resolver()->resolvers() )
    {
        string name = pap->classname();
        boost::algorithm::to_lower( name );
        m_urlHandlers[ playdar::utils::url_encode(name) ] = boost::bind( &playdar_request_handler::handle_pluginurl, this, _1, _2 );
    }
}

void 
playdar_request_handler::handle_request(const moost::http::request& req, moost::http::reply& rep)
{
    //TODO: Handle % encodings
    
    cout << "HTTP " << req.method << " " << req.uri << " " << req.origin << endl;
    if( req.origin != "127.0.0.1" &&
        req.origin != "::1" && // not tested the ipv6 check
        req.origin != "0:0:0:0:0:0:0:1" && 
        req.uri.substr(0,5) != "/sid/" )
    {
        cout << "BLOCKED. Only localhost may access non /sid/ urls." << endl;
        rep.stock_reply(moost::http::reply::unauthorized);
        return;
    }
    
    boost::tokenizer<boost::char_separator<char> > tokenizer(req.uri, boost::char_separator<char>("/?"));
    string base;
    if( tokenizer.begin() != tokenizer.end())
        base = *tokenizer.begin();

    boost::to_lower(base);
    HandlerMap::iterator handler = m_urlHandlers.find( base );
    if( handler != m_urlHandlers.end())
    {
        handler->second( req, rep );
    }
    else
    {
        rep.stock_reply(moost::http::reply::not_found);
    } 
}


/// Phase 1 - this was opened in a popup from a button in the playdar 
///           toolbar on some site that needs to authenticate
void 
playdar_request_handler::handle_auth1( const playdar_request& req,
                                       moost::http::reply& rep)
{
    if( !req.getvar_exists("website") || !req.getvar_exists("name") )
    {
        rep.stock_reply(moost::http::reply::bad_request);
        return;
    }

    string ftoken = app()->resolver()->gen_uuid();
    m_pauth->add_formtoken( ftoken );

    if (req.getvar_exists("json")) {
        // json response
        json_spirit::Object o;
        o.push_back( json_spirit::Pair( "formtoken", ftoken ));
        std::string body = json_spirit::write_formatted(o);
        rep.add_header( "Content-Length", body.length() );
        rep.write_content( body );
        rep.write_finish();
    } else {
        // webpage response
        map<string, string> vars;
        vars["<%URL%>"]="";
        if(req.getvar_exists("receiverurl"))
        {
            vars["<%URL%>"] = htmlentities(req.getvar("receiverurl"));
        }
        vars["<%FORMTOKEN%>"]=ftoken;   // ours. it's clean.
        vars["<%WEBSITE%>"]=htmlentities(req.getvar("website"));
        vars["<%NAME%>"]=htmlentities(req.getvar("name"));
        string filename = app()->conf()->get(string("www_root"), string("www")).append("/static/auth.html");
        serve_dynamic(rep, filename, vars);
    }
}


void 
playdar_request_handler::handle_auth2( const playdar_request& req, moost::http::reply& rep )
{
    
    if( !req.postvar_exists("website") ||
        !req.postvar_exists("name") ||
        !req.postvar_exists("formtoken"))
    {
        rep.stock_reply(moost::http::reply::bad_request);
        return;
    }
    
    if(m_pauth->consume_formtoken(req.postvar("formtoken")))
    {
        string tok = app()->resolver()->gen_uuid(); 
        m_pauth->create_new(tok, req.postvar("website"), req.postvar("name"), req.useragent() );
        if( !req.postvar_exists("receiverurl") || req.postvar("receiverurl")=="" )
        {
            if (req.postvar_exists("json")) {
                // json response
                json_spirit::Object o;
                o.push_back( json_spirit::Pair( "authtoken", tok ));
                std::string body = json_spirit::write_formatted(o);
                rep.add_header( "Content-Length", body.length() );
                rep.write_content( body );
                rep.write_finish();
            } else {
                // webpage response
                map<string,string> vars;
                vars["<%WEBSITE%>"]=htmlentities(req.postvar("website"));
                vars["<%NAME%>"]=htmlentities(req.postvar("name"));
                vars["<%AUTHCODE%>"]=tok;   // token is ours, it's clean.
                string filename = app()->conf()->get(string("www_root"), string("www")).append("/static/auth.na.html");
                serve_dynamic(rep, filename, vars);
            }
        }
        else
        {
            const string& recvurl = req.postvar( "receiverurl" );
            if (recvurl.find_first_of("\r\n") == string::npos) {
                // receiverurl is clean enough
                ostringstream os;
                os  << recvurl
                << ( recvurl.find( "?" ) == string::npos ? "?" : "&" )
                << "authtoken=" << tok
                << "#" << tok;
                rep.add_header( "Location", os.str() );
                rep.stock_reply(moost::http::reply::moved_permanently);
            } else {
                rep.stock_reply(moost::http::reply::bad_request);
            }
        }
    }
    else
    {
        cerr << "Invalid formtoken, not authenticating" << endl;
        rep.stock_reply(moost::http::reply::unauthorized); 
    }
}

void 
playdar_request_handler::handle_crossdomain( const playdar_request& req,
                                             moost::http::reply& rep)
{
    ostringstream os;
    os  << "<?xml version=\"1.0\"?>" << endl
        << "<!DOCTYPE cross-domain-policy SYSTEM \"http://www.macromedia.com/xml/dtds/cross-domain-policy.dtd\">"
        << "<cross-domain-policy><allow-access-from domain=\"*\" /></cross-domain-policy>" << endl;
    rep.add_header( "Content-Type", "text/xml" );
    rep.write_content( os.str() );
    rep.write_finish();
}

void 
playdar_request_handler::handle_root( const playdar_request& req,
                                      moost::http::reply& rep)
{
    ostringstream os;
    os  << "<h2>" << htmlentities(app()->conf()->name()) << "</h2>"
           "<p>"
           "Your Playdar server is running! Websites and applications that "
           "support Playdar will ask your permission, and then be able to "
           "access music you have on your machine."
           "</p>"

           "<p>"
           "For quick and dirty resolving, you can try constructing an URL like: <br/> "
           "<code>" << htmlentities(app()->conf()->httpbase()) << "/quickplay/ARTIST/ALBUM/TRACK</code><br/>"
           "</p>"

           "<p>"
           "For the real demo that uses the JSON API, check "
           "<a href=\"http://www.playdar.org/\">Playdar.org</a>"
           "</p>"

           "<p>"
           "<h3>Resolver Plugins</h3>"
           "<table>"
           "<tr style=\"font-weight: bold;\">"
           "<td>Plugin Name</td>"
           "<td>Weight</td>"
           "<td>Preference</td>"
           "<td>Target Time</td>"
           "<td>Scope</td>"
           "<td>Configuration</td>"
           "</tr>"
           ;
    unsigned short lw = 0;
    bool dupe = false;
    int i = 0;
    string bgc="";
    BOOST_FOREACH(const pa_ptr pap, app()->resolver()->resolvers())
    {
        if(pap->weight() == 0) break;
        dupe = (lw == pap->weight());
        if(lw==0) lw = pap->weight();
        if(!dupe) bgc = (i++%2==0) ? "lightgrey" : "" ;
        os  << "<tr style=\"background-color: " << bgc << "\">"
            "<td>" << htmlentities(pap->rs()->name()) << "</td>"
            "<td>" << pap->weight() << "</td>"
            "<td>" << pap->preference() << "</td>"
            "<td>" << pap->targettime() << "ms</td>"
            "<td>" << (pap->localonly()?"local":"global") << "</td>"
            "<td>";
        string name = pap->rs()->name();
        boost::algorithm::to_lower( name );
        os << "<a href=\"" << htmlentities(name) << "/config\">" 
           << htmlentities(name) << " config</a><br/></td>"
        "</tr>" << endl;
    }
    os  << "</table></p>";
    
    os  << "<p>"
           "<h3>Other Plugins</h3>"
           "<table>"
           "<tr style=\"font-weight: bold;\">"
           "<td>Plugin Name</td>"
           "<td>Configuration</td>"
           "</tr>" ;
    bgc=""; i = 0;
    BOOST_FOREACH(const pa_ptr pap, app()->resolver()->resolvers())
    {
        if(pap->weight()>0) continue;
        if(!dupe) bgc = (i++%2==0) ? "lightgrey" : "" ;
        string name = pap->rs()->name();
        boost::algorithm::to_lower( name );
        os  <<  "<tr style=\"background-color: " << bgc << "\">"
                "<td>" << htmlentities(pap->rs()->name()) << "</td>"
                "<td><a href=\"" << htmlentities(name) << "/config" <<"\">" 
                << htmlentities(name) << " config</a>" 
                "</td></tr>" << endl;
    }
    os  << "</table></p>" << endl;
    serve_body(os.str(), rep);

}

void
playdar_request_handler::handle_pluginurl( const playdar_request& req,
                                           moost::http::reply& rep )
{
    //TODO: handle script resolver urls?
    ResolverService* rs = app()->resolver()->get_resolver( req.parts()[0] );

    if( rs == 0 )
    {
        cout << "No plugin of that name found." << endl;
        rep.stock_reply(moost::http::reply::not_found);
        return;
    }

    /// Auth stuff
    string permissions = "";
    if(m_disableAuth || req.getvar_exists("auth"))
    {
        string whom;
        if(m_disableAuth || m_pauth->is_valid(req.getvar("auth"), whom) )
        {
            //cout << "AUTH: validated " << whom << endl;
            permissions = "*"; // allow all.
        }
        else
        {
            //cout << "AUTH: Invalid authtoken." << endl;
        }
    }
    else
    {
        //cout << "AUTH: no auth value provided." << endl;
    }

    playdar_response resp;
    if( permissions == "*" )
        rs->authed_http_handler( req, resp, *m_pauth ) || rs->anon_http_handler( req, resp, *m_pauth );
    else
        rs->anon_http_handler( req, resp, *m_pauth );

    if( resp.is_valid() )
        serve_body( resp, rep );
    else
        rep.stock_reply(moost::http::reply::not_found);
    
}

void 
playdar_request_handler::handle_shutdown( const playdar_request& req,
                                          moost::http::reply& rep )
{
    app()->shutdown();
}

void 
playdar_request_handler::handle_settings( const playdar_request& req,
                                          moost::http::reply& rep )
{
    
    if( req.parts().size() == 1 || req.parts()[1] == "config" )
    {
        ostringstream os;
        os  << "<h2>Configuration</h2>"
            "<p>"
            "Config options are stored as a JSON object in the file: "
            "<code>" << htmlentities(app()->conf()->filename()) << "</code>"
            "</p>"
            "<p>"
            "The contents of the file are shown below, to edit it use "
            "your favourite text editor."
            "</p>"
            "<pre>" << htmlentities(app()->conf()->str()) << "</pre>"
            ;
        serve_body(os.str(), rep);
    }
    else if( req.parts()[1] == "auth" )
    {
        ostringstream os;
        os  << "<h2>Authenticated Sites</h2>";

        if (req.getvar_exists("revoke"))
        {
            m_pauth->deauth(req.getvar("revoke"));

            if (req.getvar_exists("jsonp")) 
            {
                // handle the entire jsonp response right here:
                string content = req.getvar("jsonp") + "();\n";
                rep.set_status(moost::http::reply::ok);
                rep.add_header("Content-Type", "text/javascript; charset=utf-8");
                rep.add_header("Content-Length", content.size());
                rep.write_content(content);
                rep.write_finish();
                return;
            } 

            os  << "<p style=\"font-weight:bold;\">"
                "You have revoked access for auth-token: "
                << htmlentities(req.getvar("revoke"))
                << "</p>";
        }

        typedef map<string,string> auth_t;
        vector< auth_t > v = m_pauth->get_all_authed();
        os  << "<p>"
            "The first time a site requests access to your Playdar, "
            "you'll have a chance to allow/deny it. You can see the list "
            "of authenticated sites here, and delete any if necessary."
            "</p>"
            "<table style=\"width:95%\">"
               "<tr style=\"font-weight:bold;\">"
               "<td>Name</td>"
               "<td>Website</td>"
               "<td>Auth Code / User-Agent</td>"
               "<td>Options</td>"
              "</tr>"
            << endl;
        int i = 0;
        string formtoken = app()->resolver()->gen_uuid();
        m_pauth->add_formtoken( formtoken );
        BOOST_FOREACH( auth_t &m, v )
        {
            os  << "<tr style=\"background-color:" << ((i++%2==0)?"#ccc":"") << ";\">"
                "<td>" << htmlentities(m["name"]) << "</td>"
                "<td>" << htmlentities(m["website"]) << "</td>"
                "<td>" << htmlentities(m["token"]) << "<br/><small>"
                << htmlentities(m["ua"]) << "</small></td>"
                "<td><a href=\"/settings/auth/?revoke="  
                << htmlentities(m["token"]) << "\">Revoke</a>"
                "</td>"
                "</tr>";
        }
        os  << "</table>" << endl;

        serve_body( os.str(), rep );
    }
    else
        rep.stock_reply(moost::http::reply::bad_request);
}

string 
playdar_request_handler::handle_queries_root(const playdar_request& req)
{
    if(req.postvar_exists( "cancel_query" ) && req.postvar("cancel_query").length() && 
       req.postvar_exists( "qid" ) && req.postvar("qid").length())
    {
        app()->resolver()->cancel_query( req.postvar("qid") );
    }

    deque< query_uid > queries;
    app()->resolver()->qids(queries);

    ostringstream os;
    os  << "<h2>Current Queries (" << queries.size() << ")</h2>"
        "<table>"
        "<tr style=\"font-weight:bold;\">"
        "<td>QID</td>"
        "<td>Options</td>"
        "<td>Artist</td>"
        "<td>Album</td>"
        "<td>Track</td>"
        "<td>Origin</td>"
        "<td>Results</td>"
        "</tr>"
        ;

    
    deque< query_uid>::const_iterator it( queries.begin() );
    for(int i = 0; it != queries.end(); it++, i++)
    {
        try
        { 
            rq_ptr rq( app()->resolver()->rq(*it) );
            string bgc( i%2 ? "lightgrey" : "" );
            os  << "<tr style=\"background-color: "<< bgc << "\">";
            if(!rq) {
             os << "<td colspan=\"7\"><i>cancelled query</i></td>";
            } else if (rq->isValidTrack()) {
             os << "<td style=\"font-size:60%;\">" 
                "<a href=\"/queries/"<< htmlentities(rq->id()) <<"\">" << htmlentities(rq->id()) << "</a></td>"
                "<td style=\"align:center;\">"
                "<form method=\"post\" action=\"\" style=\"margin:0; padding:0;\">"
                "<input type=\"hidden\" name=\"qid\" value=\"" << htmlentities(rq->id()) << "\"/>"
                "<input type=\"submit\" value=\"X\" name=\"cancel_query\" style=\"margin:0; padding:0;\" title=\"Cancel and invalidate this query\"/></form>"
                "</td>"
                "<td>" << htmlentities(rq->param( "artist" ).get_str()) << "</td>"
                "<td>" << htmlentities(rq->param( "album" ).get_str()) << "</td>"
                "<td>" << htmlentities(rq->param( "track" ).get_str()) << "</td>"
                "<td>" << htmlentities(rq->from_name()) << "</td>"
                "<td " << (rq->solved()?"style=\"background-color: lightgreen;\"":"") << ">" 
                << rq->num_results() << "</td>"
                ; 
            } else {
                os << "<td><i>non-track query</i></td>"
                "<td colspan=\"6\" style=\"align:center;\">"
                "<form method=\"post\" action=\"\" style=\"margin:0; padding:0;\">"
                "<input type=\"hidden\" name=\"qid\" value=\"" << htmlentities(rq->id()) << "\"/>"
                "<input type=\"submit\" value=\"X\" name=\"cancel_query\" style=\"margin:0; padding:0;\" title=\"Cancel and invalidate this query\"/></form>"
                "</td>";
            }
            os << "</tr>";
        } catch(...) { }
    }  
    return os.str();
}

void 
playdar_request_handler::handle_queries( const playdar_request& req,
                                         moost::http::reply& rep )
{
    do
    {
        if( req.parts().size() == 1 )
        {
            cout << "Query handler, parts: " << req.parts()[0] << endl;

            const string& s = handle_queries_root(req);
            serve_body( s, rep );
            break;
        }
        else if( req.parts().size() == 2 )
        {
            cout << "Query handler, parts: " << req.parts()[0] << ", " << req.parts()[1] << endl;

            query_uid qid = req.parts()[1];
            rq_ptr rq = app()->resolver()->rq(qid);
            if(!rq || !rq->isValidTrack())
           {
               rep.stock_reply(moost::http::reply::not_found);
               return;
           }
           vector< ri_ptr > results = rq->results();

           ostringstream os;
           os  << "<h2>Query: " << qid << "</h2>"
               "<table>"
               "<tr><td>Artist</td>"
               "<td>" << htmlentities(rq->param( "artist" ).get_str()) << "</td></tr>"
               "<tr><td>Album</td>"
               "<td>" << htmlentities(rq->param( "album" ).get_str()) << "</td></tr>"
               "<tr><td>Track</td>"
               "<td>" << htmlentities(rq->param( "track" ).get_str()) << "</td></tr>"
               "</table>"
               "<h3>Results (" << results.size() << ")</h3>"
               "<table>"
               "<tr style=\"font-weight:bold;\">"
               "<td>SID</td>"
               "<td>Artist</td>"
               "<td>Album</td>"
               "<td>Track</td>"
               "<td>Dur</td>"
               "<td>Kbps</td>"
               "<td>Size</td>"
               "<td>Source</td>"
               "<td>Score</td>"
               "</tr>"
               ;
           string bgc="";
           int i = 0;
           BOOST_FOREACH(ri_ptr ri, results)
           {
               if( !ri->has_json_value<string>( "artist" ) ||
                   !ri->has_json_value<string>( "track" ))
                  continue;
               
               bgc = ++i%2 ? "lightgrey" : "";
               os  << "<tr style=\"background-color:" << bgc << "\">"
                   "<td style=\"font-size:60%\">"
                   "<a href=\"/sid/" << htmlentities(ri->id()) << "\">" << htmlentities(ri->id()) << "</a></td>"
                   "<td>" << htmlentities(ri->json_value("artist", "" ))  << "</td>"
                   "<td>" << htmlentities(ri->json_value("album", "" ))   << "</td>"
                   "<td>" << htmlentities(ri->json_value("track", "" ))   << "</td>"
                   "<td>" << htmlentities(ri->json_value("duration", "" ))<< "</td>"
                   "<td>" << htmlentities(ri->json_value("bitrate", "" )) << "</td>"
                   "<td>" << htmlentities(ri->json_value("size", "" ))    << "</td>"
                   "<td>" << htmlentities(ri->json_value("source", "" ))  << "</td>"
                   "<td>" << htmlentities(ri->json_value("score", "" ))   << "</td>"
                   "</tr>"
                   ;
           }
           os  << "</table>";
           serve_body( os.str(), rep );
           break;
        }
        else
        {
           rep.stock_reply(moost::http::reply::not_found);
        }
    }
    while( false );
}

/// serves file based on SID
void 
playdar_request_handler::handle_sid( const playdar_request& req,
                                     moost::http::reply& rep )
{
    if( req.parts().size() != 2)
    {
        rep.stock_reply(moost::http::reply::bad_request );
        return;
    }
    
    source_uid sid = req.parts()[1];
    serve_sid( rep, sid);
}

/// quick hack method for playing a song, if it can be found:
///  /quickplay/The+Beatles//Yellow+Submarine
void
playdar_request_handler::handle_quickplay( const playdar_request& req, 
                                           moost::http::reply& rep )
{
    if( req.parts().size() != 4
       || !req.parts()[1].length() || !req.parts()[3].length() )
    {
        rep.stock_reply( moost::http::reply::bad_request );
        return;
    }
    
    string artist   = req.parts()[1];
    string album    = req.parts()[2];
    string track    = req.parts()[3];
    boost::shared_ptr<ResolverQuery> rq = TrackRQBuilder::build(artist, album, track);
    rq->set_from_name(app()->conf()->name());
    rq->set_origin_local( true );
    query_uid qid = app()->resolver()->dispatch(rq);
    // wait a couple of seconds for results
    boost::xtime time; 
    boost::xtime_get(&time,boost::TIME_UTC); 
    time.sec += 2;
    boost::thread::sleep(time);
    vector< ri_ptr > results = app()->resolver()->get_results(qid);
    
    if (results.size() && !results[0]->json_value( "url", "" ).empty()) {
        json_spirit::Object ro = results[0]->get_json();
        cout << "Top result:" <<endl;
        json_spirit::write_formatted( ro, cout );
        cout << endl;
        string url = "/sid/";
        url += results[0]->id();
        rep.set_status( moost::http::reply::moved_temporarily );
        rep.add_header( "Location", url );
        rep.write_finish();
    } else {
        // we got nothing
        rep.stock_reply( moost::http::reply::not_found );
    }
}

void
playdar_request_handler::serve_body(const playdar_response& response, moost::http::reply& rep)
{
    rep.set_status( response.response_code() );
    
    typedef pair<string, string> SPair;
    BOOST_FOREACH( SPair p, response.headers() )
    {
        rep.add_header( p.first, p.second );
    }

    size_t content_length = response.str().length();
    if (content_length > 0) 
    {
        rep.add_header("Content-Length", content_length);
        rep.write_content(response.str());
    }
    rep.write_finish();
}

void
playdar_request_handler::serve_static_file(const moost::http::request& req, moost::http::reply& rep)
{
    moost::http::filesystem_request_handler frh;
    frh.doc_root(app()->conf()->get(string("www_root"), string("www")));
    frh.handle_request(req, rep);
}

// Serves the music file based on a SID 
// (from a playableitem resulting from a query)
void
playdar_request_handler::serve_sid( moost::http::reply& rep, source_uid sid)
{
    cout << "Serving SID " << sid << endl;
    ss_ptr ss = app()->resolver()->get_ss(sid);
    if(!ss)
    {
        cerr << "This SID does not exist or does not resolve to a playable item." << endl;
        rep.stock_reply(moost::http::reply::not_found);
        return;
    }
    cout << "-> " << ss->debug() << endl;

    boost::shared_ptr<HttpAsyncAdaptor> hp(new HttpAsyncAdaptor(rep.shared_from_this()));
    ss->start_reply(hp);
}


// serves a .html file from docroot, but string substitutes stuff in the vars map.
void
playdar_request_handler::serve_dynamic( moost::http::reply& rep,
                                        string filename,
                                        map<string,string> vars)
{
    ifstream ifs;
    ifs.open(filename.c_str(), ifstream::in);
    if(ifs.fail())
    {
        cerr << "FAIL" << endl;
        rep.stock_reply(moost::http::reply::not_found);
        return;
    }

    typedef std::pair<string,string> pair_t;
    ostringstream os;
    string line;
    while(std::getline(ifs, line))
    {
        BOOST_FOREACH(pair_t item, vars)
        {
            boost::replace_all(line, item.first, item.second);
        }
        os << line << endl;
    }
    rep.add_header( "Content-Type", "text/html" );
    rep.add_header( "Content-Length", os.str().length() );
    rep.write_content( os.str() );
    rep.write_finish();
}

void
playdar_request_handler::handle_comet(const playdar_request& req, moost::http::reply& rep)
{
    if (req.getvar_exists("session")) {
        const string& sessionId( req.getvar("session") );
        boost::shared_ptr<CometSession> comet(new CometSession(sessionId, rep.shared_from_this(), m_app->resolver()));
        if (comet->connect_to_resolver()) {
            rep.set_status( moost::http::reply::ok );
            rep.add_header( "Content-Type", "text/javascript; charset=utf-8" );
            static std::string startArray("[");
            rep.write_content( startArray );
        } else {
            cout << "couldn't create comet session" << endl;
            rep.stock_reply( moost::http::reply::internal_server_error );
        }
    } else {
        rep.stock_reply( moost::http::reply::bad_request );
    }
}

}
