#ifndef __RESOLVER_QUERY_H__
#define __RESOLVER_QUERY_H__

//#include "playdar/application.h"
#include "playdar/types.h"
#include "playdar/config.hpp"
#include "playdar/playable_item.hpp"

#include "json_spirit/json_spirit.h"
#include <boost/algorithm/string.hpp>
#include <boost/bind.hpp>
#include <boost/foreach.hpp>
#include <boost/function.hpp>
#include <boost/thread/mutex.hpp>
#include "time.h"

// Represents a search query to resolve a particular track
// Contains results, as they are found
using namespace std;

class ResolverQuery
{
public:
    ResolverQuery(string art, string alb, string trk)
        : m_artist(art), m_album(alb), m_track(trk), m_mode("normal"), 
          m_solved(false), m_cancelled(false)
    {
        boost::trim(m_artist);
        boost::trim(m_album);
        boost::trim(m_track);
        // set initial "last access" time:
        time(&m_atime);
    }
    
    // is this a valid / well formed query?
    bool valid() const
    {
        time(&m_atime);
        return m_artist.length()>0 && m_track.length()>0;
    }
    
    /// when was this query last "used"
    time_t atime() const
    {
        return m_atime;
    }

    ResolverQuery(){}

    ~ResolverQuery()
    {
        cout << "DTOR, resolver query: " << id() << " - " << str() << endl;
    }
    
    /// performs cleanup, query not needed any more.
    /// object should destruct soon after this is called, once any lingering references are released.
    void cancel()
    {
        m_cancelled = true;
        m_callbacks.clear();
        cout << "RQ::cancel() for " << id() << endl;
    }
    
    /// returns true if query cancelled and pending destruction. (hint, release your ptr)
    bool cancelled() const { return m_cancelled; }
    
    json_spirit::Object get_json() const
    {
        time(&m_atime);
        using namespace json_spirit;
        Object j;
        j.push_back( Pair("_msgtype", "rq")   );
        j.push_back( Pair("qid",    id())     );
        j.push_back( Pair("artist", artist()) );
        j.push_back( Pair("album",  album())  );
        j.push_back( Pair("track",  track())  );
        j.push_back( Pair("mode",  mode())  );
        j.push_back( Pair("solved",  solved())  );
        j.push_back( Pair("from_name",  from_name())  );
        return j;
    }
    
    static boost::shared_ptr<ResolverQuery> from_json(json_spirit::Object qryobj)
    {
        string qid, artist, album, track, mode, from_name;
        
        using namespace json_spirit;
        map<string,Value> qryobj_map;
        obj_to_map(qryobj, qryobj_map);
        
        if(qryobj_map.find("artist")!=qryobj_map.end()) 
            artist = qryobj_map["artist"].get_str();
        if(qryobj_map.find("track")!=qryobj_map.end()) 
            track = qryobj_map["track"].get_str();
        if(qryobj_map.find("album")!=qryobj_map.end()) 
            album = qryobj_map["album"].get_str();
        
        if(qryobj_map.find("qid")!=qryobj_map.end()) 
            qid = qryobj_map["qid"].get_str();
        if(qryobj_map.find("mode")!=qryobj_map.end()) 
            mode = qryobj_map["mode"].get_str();
        if(qryobj_map.find("from_name")!=qryobj_map.end()) 
            from_name = qryobj_map["from_name"].get_str();
                    
        if(artist.length()==0 || track.length()==0) throw;
        
        boost::shared_ptr<ResolverQuery>    
            rq(new ResolverQuery(artist, album, track));
        if(mode.length()) rq->set_mode(mode);
        if(qid.length())  rq->set_id(qid);
        if(from_name.length())  rq->set_from_name(from_name);
        return rq;
    }
    
    void set_id(query_uid q)
    {
        m_uuid = q;
    }
    
    void set_from_name(string s) { m_from_name = s; }
    
    void set_mode(string s){ m_mode = s; }
    
    // create uuid on demand if one wasn't specified by now:
    const query_uid & id() const
    { 
        if(!m_uuid.length())
        {
            m_uuid = playdar::Config::gen_uuid();
        }
        return m_uuid; 
    }
    
    string str() const
    {
        string s = artist();
        s+=" - ";
        s+=album();
        s+=" - ";
        s+=track();
        return s;
    }

    size_t num_results() const
    {
        return m_results.size();
    }

    vector< boost::shared_ptr<PlayableItem> > results()
    {
        time(&m_atime);
        // sort results on score/preference.
        boost::function
                    < bool 
                    (   const boost::shared_ptr<PlayableItem> &, 
                        const boost::shared_ptr<PlayableItem> &
                    ) > sortfun = 
                    boost::bind(&ResolverQuery::sorter, this, _1, _2);
        
        boost::mutex::scoped_lock lock(m_mut);
        sort(m_results.begin(), m_results.end(), sortfun);
        return m_results; 
    }

    bool sorter(const boost::shared_ptr<PlayableItem> & lhs, const boost::shared_ptr<PlayableItem> & rhs)
    {
        // if equal scores, prefer item with higher preference (ie, network reliability)
        //if(lhs->score() == rhs->score()) return lhs->preference() > rhs->preference();
        // TODO: the one that came from the resolverservice with the
        // highest weight should win if there is a tie.
        return lhs->score() > rhs->score();
    }

    void add_result(boost::shared_ptr<PlayableItem> pip) 
    { 
        //cout << "RQ.add_result: "<< pip->score() <<"\t"
        //     << pip->artist() << " - " << pip->track() << endl;
        {
            boost::mutex::scoped_lock lock(m_mut);
            m_results.push_back(pip); 
        }
        // decide if this result "solves" the query:
        // for now just assume score of 1 means solved.
        if(pip->score() == 1.0) 
        {
//            cout << "SOLVED " << id() << endl;   
            m_solved = true;
        }
        // fire callbacks:
        BOOST_FOREACH(rq_callback_t & cb, m_callbacks)
        {
            cb(id(), pip);
        }
    }
    
    void register_callback(rq_callback_t cb)
    {
        m_callbacks.push_back( cb );
    }
    
    bool solved()   const { return m_solved; }
    string artist() const { return m_artist; }
    string album()  const { return m_album; }
    string track()  const { return m_track; }
    string mode()   const { return m_mode;  }
    string from_name() const { return m_from_name;  }

    
    
    
private:
    vector< boost::shared_ptr<PlayableItem> > m_results;
    string      m_artist;
    string      m_album;
    string      m_track;
    string      m_from_name;
        
    // list of functors to fire on new result:
    vector<rq_callback_t> m_callbacks;
    string      m_mode; // only other options here is "spamme" atm.
    // mutable because created on-demand the first time it's needed
    mutable query_uid   m_uuid;
    boost::mutex m_mut;
    // set to true once we get a decent result
    bool m_solved;
    // set to true if trying to cancel/delete this query (if so, don't bother working with it)
    bool m_cancelled;
    // last access time (used to know if this query is stale and can be deleted)
    // mutable: it's auto-updated to mark the atime in various places.
    mutable time_t m_atime; 

};

#endif

