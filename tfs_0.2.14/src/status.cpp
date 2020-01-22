//////////////////////////////////////////////////////////////////////
// OpenTibia - an opensource roleplaying game
//////////////////////////////////////////////////////////////////////
// Status
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

#include <libxml/xmlmemory.h>
#include <libxml/parser.h>

#include "status.h"
#include "configmanager.h"
#include "game.h"
#include "connection.h"
#include "networkmessage.h"
#include "outputmessage.h"
#include "tools.h"
#include "resources.h"

#ifndef WIN32
	#define SOCKET_ERROR -1
	#define INVALID_SOCKET -1
#endif

extern ConfigManager g_config;
extern Game g_game;

#ifdef __ENABLE_SERVER_DIAGNOSTIC__
uint32_t ProtocolStatus::protocolStatusCount = 0;
#endif

enum RequestedInfo_t
{
	REQUEST_BASIC_SERVER_INFO = 0x01,
	REQUEST_OWNER_SERVER_INFO = 0x02,
	REQUEST_MISC_SERVER_INFO = 0x04,
	REQUEST_PLAYERS_INFO = 0x08,
	REQUEST_MAP_INFO = 0x10,
	REQUEST_EXT_PLAYERS_INFO = 0x20,
	REQUEST_PLAYER_STATUS_INFO = 0x40,
	REQUEST_SERVER_SOFTWARE_INFO = 0x80
};

std::map<uint32_t, int64_t> ProtocolStatus::ipConnectMap;

void ProtocolStatus::onRecvFirstMessage(NetworkMessage& msg)
{
	uint32_t ip = getIP();
	if(ip != 0x0100007F)
	{
		std::string ipStr = convertIPToString(ip);
		if(ipStr != g_config.getString(ConfigManager::IP) && !g_game.isInWhitelist(ipStr))
		{
			if(g_game.isInBlacklist(ipStr))
			{
				getConnection()->closeConnection();
				return;
			}

			std::map<uint32_t, int64_t>::const_iterator it = ipConnectMap.find(ip);
			if(it != ipConnectMap.end())
			{
				if(OTSYS_TIME() < (it->second + g_config.getNumber(ConfigManager::STATUSQUERY_TIMEOUT)))
				{
					getConnection()->closeConnection();
					return;
				}
			}
		}
	}
	ipConnectMap[ip] = OTSYS_TIME();

	switch(msg.GetByte())
	{
		//XML info protocol
		case 0xFF:
		{
			if(msg.GetString(4) == "info")
			{
				OutputMessage_ptr output = OutputMessagePool::getInstance()->getOutputMessage(this, false);
				if(output)
				{
					TRACK_MESSAGE(output);
					Status* status = Status::getInstance();
					std::string str = status->getStatusString();
					output->AddBytes(str.c_str(), str.size());
					setRawMessages(true); // we dont want the size header, nor encryption
					OutputMessagePool::getInstance()->send(output);
				}
			}
			break;
		}

		//Another ServerInfo protocol
		case 0x01:
		{
			uint32_t requestedInfo = msg.GetU16(); //Only a Byte is necessary, though we could add new infos here
			OutputMessage_ptr output = OutputMessagePool::getInstance()->getOutputMessage(this, false);
			if(output)
			{
				TRACK_MESSAGE(output);
				Status* status = Status::getInstance();
				status->getInfo(requestedInfo, output, msg);
				OutputMessagePool::getInstance()->send(output);
			}
			break;
		}
		default:
			break;
	}
	getConnection()->closeConnection();
}

#ifdef __DEBUG_NET_DETAIL__
void ProtocolStatus::deleteProtocolTask()
{
	std::cout << "Deleting ProtocolStatus" << std::endl;
	Protocol::deleteProtocolTask();
}
#endif

Status::Status()
{
	m_playersOnline = 0;
	m_playersMax = 0;
	m_start = OTSYS_TIME();
}

void Status::addPlayer()
{
	m_playersOnline++;
}

void Status::removePlayer()
{
	m_playersOnline--;
}

template <typename T>
void addXMLProperty(xmlNodePtr p, const std::string& tag, T val)
{
	std::ostringstream os;
	os << val;
	xmlSetProp(p, (const xmlChar*)tag.c_str(), (const xmlChar*)os.str().c_str());
}

void addXMLProperty(xmlNodePtr p, const std::string& tag, const char* val)
{
	xmlSetProp(p, (const xmlChar*)tag.c_str(), (const xmlChar*)val);
}

void addXMLProperty(xmlNodePtr p, const std::string& tag, const std::string& val)
{
	xmlSetProp(p, (const xmlChar*)tag.c_str(), (const xmlChar*)val.c_str());
}

std::string Status::getStatusString() const
{
	std::string xml;

	xmlDocPtr doc;
	xmlNodePtr p, root;

	doc = xmlNewDoc((const xmlChar*)"1.0");
	doc->children = xmlNewDocNode(doc, NULL, (const xmlChar*)"tsqp", NULL);
	root = doc->children;

	xmlSetProp(root, (const xmlChar*)"version", (const xmlChar*)"1.0");

	p = xmlNewNode(NULL, (const xmlChar*)"serverinfo");
	addXMLProperty(p, "uptime", getUptime());
	addXMLProperty(p, "ip", g_config.getString(ConfigManager::IP));
	addXMLProperty(p, "servername", g_config.getString(ConfigManager::SERVER_NAME));
	addXMLProperty(p, "port", g_config.getNumber(ConfigManager::LOGIN_PORT));
	addXMLProperty(p, "location", g_config.getString(ConfigManager::LOCATION).c_str());
	addXMLProperty(p, "url", g_config.getString(ConfigManager::URL).c_str());
	addXMLProperty(p, "server", STATUS_SERVER_NAME);
	addXMLProperty(p, "version", STATUS_SERVER_VERSION);
	addXMLProperty(p, "client", STATUS_SERVER_PROTOCOL);
	xmlAddChild(root, p);

	p = xmlNewNode(NULL, (const xmlChar*)"owner");
	addXMLProperty(p, "name", g_config.getString(ConfigManager::OWNER_NAME));
	addXMLProperty(p, "email", g_config.getString(ConfigManager::OWNER_EMAIL));
	xmlAddChild(root, p);

	p = xmlNewNode(NULL,(const xmlChar*)"players");
	addXMLProperty(p, "online", m_playersOnline);
	addXMLProperty(p, "max", m_playersMax);
	addXMLProperty(p, "peak", g_game.getLastPlayersRecord());
	xmlAddChild(root, p);

	p = xmlNewNode(NULL,(const xmlChar*)"monsters");
	addXMLProperty(p, "total", g_game.getMonstersOnline());
	xmlAddChild(root, p);

	uint32_t mapWidth, mapHeight;
	g_game.getMapDimensions(mapWidth, mapHeight);

	p = xmlNewNode(NULL,(const xmlChar*)"map");
	addXMLProperty(p, "name", m_mapName);
	addXMLProperty(p, "author", m_mapAuthor);
	addXMLProperty(p, "width", mapWidth);
	addXMLProperty(p, "height", mapHeight);
	xmlAddChild(root, p);

	xmlNewTextChild(root, NULL, (const xmlChar*)"motd", (const xmlChar*)g_config.getString(ConfigManager::MOTD).c_str());

	xmlChar* s = NULL;
	int32_t len = 0;
	xmlDocDumpMemory(doc, (xmlChar**)&s, &len);

	if(s)
		xml = std::string((char*)s, len);
	else
		xml = "";

	xmlFreeOTSERV(s);
	xmlFreeDoc(doc);
	return xml;
}

void Status::getInfo(uint32_t requestedInfo, OutputMessage_ptr output, NetworkMessage& msg) const
{
	if(requestedInfo & REQUEST_BASIC_SERVER_INFO)
	{
		output->AddByte(0x10);
		output->AddString(g_config.getString(ConfigManager::SERVER_NAME).c_str());
		output->AddString(g_config.getString(ConfigManager::IP).c_str());

		std::ostringstream ss;
		ss << g_config.getNumber(ConfigManager::LOGIN_PORT);
		output->AddString(ss.str());
	}

	if(requestedInfo & REQUEST_OWNER_SERVER_INFO)
	{
		output->AddByte(0x11);
		output->AddString(g_config.getString(ConfigManager::OWNER_NAME).c_str());
		output->AddString(g_config.getString(ConfigManager::OWNER_EMAIL).c_str());
	}

	if(requestedInfo & REQUEST_MISC_SERVER_INFO)
	{
		uint64_t running = getUptime();
		output->AddByte(0x12);
		output->AddString(g_config.getString(ConfigManager::MOTD).c_str());
		output->AddString(g_config.getString(ConfigManager::LOCATION).c_str());
		output->AddString(g_config.getString(ConfigManager::URL).c_str());
		output->AddU32((uint32_t)(running >> 32));
		output->AddU32((uint32_t)(running));
	}

	if(requestedInfo & REQUEST_PLAYERS_INFO)
	{
		output->AddByte(0x20);
		output->AddU32(m_playersOnline);
		output->AddU32(m_playersMax);
		output->AddU32(g_game.getLastPlayersRecord());
	}

	if(requestedInfo & REQUEST_MAP_INFO)
	{
		output->AddByte(0x30);
		output->AddString(m_mapName.c_str());
		output->AddString(m_mapAuthor.c_str());
		uint32_t mapWidth, mapHeight;
		g_game.getMapDimensions(mapWidth, mapHeight);
		output->AddU16(mapWidth);
		output->AddU16(mapHeight);
	}

	if(requestedInfo & REQUEST_EXT_PLAYERS_INFO)
	{
		output->AddByte(0x21); // players info - online players list
		output->AddU32(m_playersOnline);
		for(AutoList<Player>::listiterator it = Player::listPlayer.list.begin(); it != Player::listPlayer.list.end(); ++it)
		{
			//Send the most common info
			output->AddString(it->second->getName());
			output->AddU32(it->second->getLevel());
		}
	}

	if(requestedInfo & REQUEST_PLAYER_STATUS_INFO)
	{
		output->AddByte(0x22); // players info - online status info of a player
		const std::string name = msg.GetString();
		if(g_game.getPlayerByName(name) != NULL)
			output->AddByte(0x01);
		else
			output->AddByte(0x00);
	}

	if(requestedInfo & REQUEST_SERVER_SOFTWARE_INFO)
	{
		output->AddByte(0x23); // server software info
		output->AddString(STATUS_SERVER_NAME);
		output->AddString(STATUS_SERVER_VERSION);
		output->AddString(STATUS_SERVER_PROTOCOL);
	}
	return;
}

uint64_t Status::getUptime() const
{
	return (OTSYS_TIME() - m_start) / 1000;
}
