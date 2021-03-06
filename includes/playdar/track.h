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
#ifndef __TRACK_H__
#define __TRACK_H__

#include <string>
#include "types.h"

namespace playdar {

class Track
{
public:
    Track(int id, const std::string& name, artist_ptr ap)
        :m_id(id), m_name(name), m_artist(ap)
    {}
    
    int id()                  const { return m_id; }
    const std::string& name() const { return m_name; }
    artist_ptr artist()       const { return m_artist; }
    
private:

    int m_id;
    std::string m_name;
    artist_ptr m_artist;
};

}

#endif
