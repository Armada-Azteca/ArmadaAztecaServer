//////////////////////////////////////////////////////////////////////
// OpenTibia - an opensource roleplaying game
//////////////////////////////////////////////////////////////////////
// Mounts
//////////////////////////////////////////////////////////////////////
// This program is free software; you can redistribute it and/or
// modify it under the terms of the GNU General Public License
// as published by the Free Software Foundation; either version 2
// of the License, or (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with this program; if not, write to the Free Software Foundation,
// Inc., 59 Temple Place - Suite 330, Boston, MA  02111-1307, USA.
//////////////////////////////////////////////////////////////////////

#include "otpch.h"

#include "mounts.h"
#include "tools.h"

Mount::Mount(uint8_t _id, uint16_t _clientId, std::string _name, int32_t _speed, bool _premium)
{
	id = _id;
	clientId = _clientId;
	name = _name;
	speed = _speed;
	premium = _premium;
}

bool Mount::isTamed(Player* player) const
{
	if(!player)
		return false;

	if(player->isAccessPlayer())
		return true;

	if(premium && !player->isPremium())
		return false;

	uint8_t tmpId = id - 1;

	int32_t value = 0;
	if(!player->getStorageValue(PSTRG_MOUNTS_RANGE_START + (tmpId / 31), value))
		return false;

	int32_t tmp = (1 << (tmpId % 31));
	return (tmp & value) == tmp;
}

Mounts::~Mounts()
{
	for(MountsList::iterator it = mounts.begin(), end = mounts.end(); it != end; ++it)
		delete (*it);

	mounts.clear();
}

bool Mounts::reload()
{
	for(MountsList::iterator it = mounts.begin(), end = mounts.end(); it != end; ++it)
		delete (*it);

	mounts.clear();
	return loadFromXml();
}

bool Mounts::loadFromXml()
{
	xmlDocPtr doc = xmlParseFile("data/XML/mounts.xml");
	if(!doc)
		return false;

	xmlNodePtr root, p;
	root = xmlDocGetRootElement(doc);
	if(xmlStrcmp(root->name,(const xmlChar*)"mounts") != 0)
	{
		xmlFreeDoc(doc);
		return false;
	}

	int32_t intValue;
	std::string strValue;
	p = root->children;
	while(p)
	{
		if(xmlStrcmp(p->name, (const xmlChar*)"mount") == 0)
		{
			int8_t id = 0;
			int16_t clientid = 0;
			std::string name = "";
			int32_t speed = 0;
			bool premium = true;
			if(readXMLInteger(p, "id", intValue))
				id = intValue;

			if(readXMLInteger(p, "clientid", intValue))
				clientid = intValue;

			if(readXMLString(p, "name", strValue))
				name = strValue;

			if(readXMLInteger(p, "speed", intValue))
				speed = intValue;

			if(readXMLString(p, "premium", strValue))
				premium = booleanString(strValue);

			mounts.push_back(new Mount(id, clientid, name, speed, premium));
		}
		p = p->next;
	}
	xmlFreeDoc(doc);
	return true;
}

Mount* Mounts::getMountByID(uint8_t id)
{
	for(MountsList::iterator it = mounts.begin(), end = mounts.end(); it != end; ++it)
	{
		if((*it)->getID() == id)
			return (*it);
	}
	return NULL;
}

Mount* Mounts::getMountByClientID(uint16_t clientId)
{
	for(MountsList::iterator it = mounts.begin(), end = mounts.end(); it != end; ++it)
	{
		if((*it)->getClientID() == clientId)
			return (*it);
	}
	return NULL;
}
