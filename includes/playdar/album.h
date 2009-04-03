#ifndef __ALBUM_H__
#define __ALBUM_H__

#include <string>

class Album
{
public:
    Album(int id, const std::string& name, artist_ptr ap)
        :m_id(id), m_name(name), m_artist(ap)
    {}
    
    int id() const                      { return m_id; }
    const std::string& name() const     { return m_name; }
    artist_ptr artist() const           { return m_artist; }
    
    
private:
    int m_id;
    std::string m_name;
    artist_ptr m_artist;
};
#endif
