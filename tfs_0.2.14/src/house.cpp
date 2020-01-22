//////////////////////////////////////////////////////////////////////
// OpenTibia - an opensource roleplaying game
//////////////////////////////////////////////////////////////////////
//
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

#include <algorithm>

#include "house.h"
#include "iologindata.h"
#include "game.h"
#include "town.h"
#include "configmanager.h"
#include "tools.h"
#include "beds.h"

extern ConfigManager g_config;
extern Game g_game;

House::House(uint32_t _houseid) :
transfer_container(ITEM_LOCKER1)
{
	isLoaded = false;
	houseName = "Forgotten headquarter (Flat 1, Area 42)";
	houseOwner = 0;
	posEntry.x = 0;
	posEntry.y = 0;
	posEntry.z = 0;
	paidUntil = 0;
	houseid = _houseid;
	rentWarnings = 0;
	rent = 0;
	townid = 0;
	transferItem = NULL;
}

House::~House()
{
	//
}

void House::addTile(HouseTile* tile)
{
	tile->setFlag(TILESTATE_PROTECTIONZONE);
	houseTiles.push_back(tile);
}

void House::setHouseOwner(uint32_t guid, Player* player/* = NULL*/)
{
	if(isLoaded && houseOwner == guid)
		return;

	isLoaded = true;

	if(houseOwner)
	{
		//send items to depot
		if(player)
			transferToDepot(player);
		else
			transferToDepot();

		PlayerVector toKick;
		for(HouseTileList::iterator it = houseTiles.begin(); it != houseTiles.end(); ++it)
		{
			if(const CreatureVector* creatures = (*it)->getCreatures())
			{
				for(CreatureVector::const_iterator cit = creatures->begin(), cend = creatures->end(); cit != cend; ++cit)
				{
					Player* player = (*cit)->getPlayer();
					if(player)
						toKick.push_back(player);
				}
			}
		}

		while(!toKick.empty())
		{
			Player* c = toKick.back();
			toKick.pop_back();
			kickPlayer(NULL, c->getName());
		}

		// we need to remove players from beds
		HouseBedItemList::iterator bit;
		for(bit = bedsList.begin(); bit != bedsList.end(); ++bit)
		{
			if((*bit)->getSleeper() != 0)
				(*bit)->wakeUp(NULL);
		}

		//clean access lists
		houseOwner = 0;
		setAccessList(SUBOWNER_LIST, "");
		setAccessList(GUEST_LIST, "");

		for(HouseDoorList::iterator it = doorList.begin(); it != doorList.end(); ++it)
			(*it)->setAccessList("");

		//reset paid date
		paidUntil = 0;
		rentWarnings = 0;
	}

	std::string name;
	if(guid != 0 && IOLoginData::getInstance()->getNameByGuid(guid, name))
	{
		houseOwner = guid;
		houseOwnerName = name;
	}
	updateDoorDescription();
}

void House::updateDoorDescription()
{
	std::ostringstream ss;
	if(houseOwner != 0)
		ss << "It belongs to house '" << houseName << "'. " << houseOwnerName << " owns this house.";
	else
	{
		int32_t housePrice = 0;
		for(HouseTileList::iterator it = getHouseTileBegin(), end = getHouseTileEnd(); it != end; ++it)
			housePrice += g_config.getNumber(ConfigManager::HOUSE_PRICE);

		ss << "It belongs to house '" << houseName << "'. Nobody owns this house. It costs " << housePrice << " gold coins.";
	}

	HouseDoorList::iterator it;
	for(it = doorList.begin(); it != doorList.end(); ++it)
		(*it)->setSpecialDescription(ss.str());
}

AccessHouseLevel_t House::getHouseAccessLevel(const Player* player)
{
	if(!player)
		return HOUSE_OWNER;

	if(player->hasFlag(PlayerFlag_CanEditHouses))
		return HOUSE_OWNER;

	if(player->getGUID() == houseOwner)
		return HOUSE_OWNER;

	if(subOwnerList.isInList(player))
		return HOUSE_SUBOWNER;

	if(guestList.isInList(player))
		return HOUSE_GUEST;

	return HOUSE_NO_INVITED;
}

bool House::kickPlayer(Player* player, const std::string& name)
{
	Player* kickingPlayer = g_game.getPlayerByName(name);
	if(kickingPlayer)
	{
		HouseTile* houseTile = dynamic_cast<HouseTile*>(kickingPlayer->getTile());
		if(houseTile && houseTile->getHouse() == this)
		{
			if(getHouseAccessLevel(player) >= getHouseAccessLevel(kickingPlayer) && !kickingPlayer->hasFlag(PlayerFlag_CanEditHouses))
			{
				Position oldPosition = kickingPlayer->getPosition();
				if(g_game.internalTeleport(kickingPlayer, getEntryPosition()) == RET_NOERROR)
				{
					g_game.addMagicEffect(oldPosition, NM_ME_POFF);
					g_game.addMagicEffect(getEntryPosition(), NM_ME_TELEPORT);
				}
				return true;
			}
		}
	}
	return false;
}

void House::setAccessList(uint32_t listId, const std::string& textlist)
{
	if(listId == GUEST_LIST)
		guestList.parseList(textlist);
	else if(listId == SUBOWNER_LIST)
		subOwnerList.parseList(textlist);
	else
	{
		Door* door = getDoorByNumber(listId);
		if(door)
			door->setAccessList(textlist);
		else
		{
			#ifdef __DEBUG_HOUSES__
			std::cout << "Failure: [House::setAccessList] door == NULL, listId = " << listId <<std::endl;
			#endif
		}
		//We dont have kick anyone
		return;
	}

	//kick uninvited players
	typedef std::list<Player*> KickPlayerList;
	KickPlayerList kickList;
	HouseTileList::iterator it;
	for(it = houseTiles.begin(); it != houseTiles.end(); ++it)
	{
		HouseTile* hTile = *it;
		if(CreatureVector* creatures = hTile->getCreatures())
		{
			CreatureVector::iterator cit;
			for(cit = creatures->begin(); cit != creatures->end(); ++cit)
			{
				Player* player = (*cit)->getPlayer();
				if(player && isInvited(player) == false)
					kickList.push_back(player);
			}
		}
	}

	KickPlayerList::iterator itkick;
	for(itkick = kickList.begin(); itkick != kickList.end(); ++itkick)
	{
		if(g_game.internalTeleport(*itkick, getEntryPosition()) == RET_NOERROR)
			g_game.addMagicEffect(getEntryPosition(), NM_ME_TELEPORT);
	}
}

bool House::transferToDepot()
{
	if(townid == 0 || houseOwner == 0)
		return false;

	std::string ownerName;
	if(!IOLoginData::getInstance()->getNameByGuid(houseOwner, ownerName))
		return false;

	Player* player = g_game.getPlayerByName(ownerName);
	if(!player)
	{
		player = new Player(ownerName, NULL);
		if(!IOLoginData::getInstance()->loadPlayer(player, ownerName))
		{
#ifdef __DEBUG__
			std::cout << "Failure: [House::transferToDepot], can not load player: " << ownerName << std::endl;
#endif
			delete player;
			return false;
		}
	}

	transferToDepot(player);

	if(player->isOffline())
	{
		IOLoginData::getInstance()->savePlayer(player, true);
		delete player;
	}
	return true;
}

bool House::transferToDepot(Player* player)
{
	if(townid == 0 || houseOwner == 0)
		return false;

	Depot* depot = player->getDepot(townid, true);

	std::list<Item*> moveItemList;
	Container* tmpContainer = NULL;
	Item* item = NULL;

	for(HouseTileList::iterator it = houseTiles.begin(); it != houseTiles.end(); ++it)
	{
		if(const TileItemVector* items = (*it)->getItemList())
		{
			for(ItemVector::const_iterator iit = items->begin(), iend = items->end(); iit != iend; ++iit)
			{
				item = (*iit);
				if(item->isPickupable())
					moveItemList.push_back(item);
				else if((tmpContainer = item->getContainer()))
				{
					for(ItemList::const_iterator cit = tmpContainer->getItems(); cit != tmpContainer->getEnd(); ++cit)
						moveItemList.push_back(*cit);
				}
			}
		}
	}

	if(!moveItemList.empty())
		player->setDepotChange(true);

	for(std::list<Item*>::iterator it = moveItemList.begin(); it != moveItemList.end(); ++it)
	{
		g_game.internalMoveItem((*it)->getParent(), depot->getInbox(), INDEX_WHEREEVER,
			(*it), (*it)->getItemCount(), NULL, FLAG_NOLIMIT);
	}
	return true;
}

bool House::getAccessList(uint32_t listId, std::string& list) const
{
	if(listId == GUEST_LIST)
	{
		guestList.getList(list);
		return true;
	}
	else if(listId == SUBOWNER_LIST)
	{
		subOwnerList.getList(list);
		return true;
	}
	else
	{
		Door* door = getDoorByNumber(listId);
		if(door)
			return door->getAccessList(list);
		else
		{
			#ifdef __DEBUG_HOUSES__
			std::cout << "Failure: [House::getAccessList] door == NULL, listId = " << listId <<std::endl;
			#endif
			return false;
		}
	}
	return false;
}

bool House::isInvited(const Player* player)
{
	return getHouseAccessLevel(player) != HOUSE_NO_INVITED;
}

void House::addDoor(Door* door)
{
	door->useThing2();
	doorList.push_back(door);
	door->setHouse(this);
	updateDoorDescription();
}

void House::removeDoor(Door* door)
{
	HouseDoorList::iterator it = std::find(doorList.begin(), doorList.end(), door);
	if(it != doorList.end())
	{
		(*it)->releaseThing2();
		doorList.erase(it);
	}
}

void House::addBed(BedItem* bed)
{
	bedsList.push_back(bed);
	bed->setHouse(this);
}

Door* House::getDoorByNumber(uint32_t doorId)
{
	HouseDoorList::iterator it;
	for(it = doorList.begin(); it != doorList.end(); ++it)
	{
		if((*it)->getDoorId() == doorId)
			return *it;
	}
	return NULL;
}

Door* House::getDoorByNumber(uint32_t doorId) const
{
	HouseDoorList::const_iterator it;
	for(it = doorList.begin(); it != doorList.end(); ++it)
	{
		if((*it)->getDoorId() == doorId)
			return *it;
	}
	return NULL;
}

Door* House::getDoorByPosition(const Position& pos)
{
	for(HouseDoorList::iterator it = doorList.begin(); it != doorList.end(); ++it)
	{
		if((*it)->getPosition() == pos)
			return *it;
	}
	return NULL;
}

bool House::canEditAccessList(uint32_t listId, const Player* player)
{
	switch(getHouseAccessLevel(player))
	{
		case HOUSE_OWNER:
			return true;

		case HOUSE_SUBOWNER:
			return listId == GUEST_LIST;

		default:
			return false;
	}
}

HouseTransferItem* House::getTransferItem()
{
	if(transferItem != NULL)
		return NULL;

	transfer_container.setParent(NULL);
	transferItem = HouseTransferItem::createHouseTransferItem(this);
	transfer_container.__addThing(transferItem);
	return transferItem;
}

void House::resetTransferItem()
{
	if(transferItem)
	{
		Item* tmpItem = transferItem;
		transferItem = NULL;
		transfer_container.setParent(NULL);

		transfer_container.__removeThing(tmpItem, tmpItem->getItemCount());
		g_game.FreeThing(tmpItem);
	}
}

HouseTransferItem* HouseTransferItem::createHouseTransferItem(House* house)
{
	HouseTransferItem* transferItem = new HouseTransferItem(house);
	transferItem->useThing2();
	transferItem->setID(ITEM_DOCUMENT_RO);
	transferItem->setSubType(1);
	std::ostringstream ss;
	ss << "It is a house transfer document for '" << house->getName() << "'.";
	transferItem->setSpecialDescription(ss.str());
	return transferItem;
}

bool HouseTransferItem::onTradeEvent(TradeEvents_t event, Player* owner)
{
	House* house;
	switch(event)
	{
		case ON_TRADE_TRANSFER:
		{
			house = getHouse();
			if(house)
				house->executeTransfer(this, owner);

			g_game.internalRemoveItem(this, 1);
			break;
		}

		case ON_TRADE_CANCEL:
		{
			house = getHouse();
			if(house)
				house->resetTransferItem();

			break;
		}

		default:
			break;
	}

	return true;
}

bool House::executeTransfer(HouseTransferItem* item, Player* newOwner)
{
	if(transferItem != item)
		return false;

	setHouseOwner(newOwner->getGUID());
	transferItem = NULL;
	return true;
}

AccessList::AccessList()
{
	//
}

AccessList::~AccessList()
{
	//
}

bool AccessList::parseList(const std::string& _list)
{
	playerList.clear();
	guildList.clear();
	expressionList.clear();
	regExList.clear();
	list = _list;
	if(_list == "")
		return true;

	std::istringstream listStream(_list);
	std::string line;
	while(getline(listStream, line))
	{
		trimString(line);
		trim_left(line, "\t");
		trim_right(line, "\t");
		trimString(line);

		std::transform(line.begin(), line.end(), line.begin(), tolower);
		if(line.substr(0,1) == "#" || line.length() > 100)
			continue;

		if(line.find("@") != std::string::npos)
		{
			std::string::size_type pos = line.find("@");
			addGuild(line.substr(pos + 1), "");
		}
		else if(line.find("!") != std::string::npos || line.find("*") != std::string::npos || line.find("?") != std::string::npos)
			addExpression(line);
		else
			addPlayer(line);
	}
	return true;
}

bool AccessList::addPlayer(std::string& name)
{
	uint32_t guid;
	std::string dbName = name;
	if(IOLoginData::getInstance()->getGuidByName(guid, dbName))
	{
		if(playerList.find(guid) == playerList.end())
		{
			playerList.insert(guid);
			return true;
		}
	}
	return false;
}

bool AccessList::addGuild(const std::string& guildName, const std::string& rank)
{
	uint32_t guildId;
	if(IOGuild::getInstance()->getGuildIdByName(guildId, guildName))
	{
		if(guildId != 0 && guildList.find(guildId) == guildList.end())
		{
			guildList.insert(guildId);
			return true;
		}
	}
	return false;
}

bool AccessList::addExpression(const std::string& expression)
{
	ExpressionList::iterator it;
	for(it = expressionList.begin(); it != expressionList.end(); ++it)
	{
		if((*it) == expression)
			return false;
	}

	std::string outExp;
	std::string metachars = ".[{}()\\+|^$";
	for(std::string::const_iterator it = expression.begin(); it != expression.end(); ++it)
	{
		if(metachars.find(*it) != std::string::npos)
			outExp += "\\";
		outExp += (*it);
	}

	replaceString(outExp, "*", ".*");
	replaceString(outExp, "?", ".?");

	try
	{
		if(outExp.length() > 0)
		{
			expressionList.push_back(outExp);
			if(outExp.substr(0,1) == "!")
			{
				if(outExp.length() > 1)
					regExList.push_front(std::make_pair(boost::regex(outExp.substr(1)), false));
			}
			else
				regExList.push_back(std::make_pair(boost::regex(outExp), true));
		}
	}
	catch(...){}
	return true;
}

bool AccessList::isInList(const Player* player)
{
	RegExList::iterator it;
	std::string name = player->getName();
	boost::cmatch what;

	std::transform(name.begin(), name.end(), name.begin(), tolower);
	for(it = regExList.begin(); it != regExList.end(); ++it)
	{
		if(boost::regex_match(name.c_str(), what, it->first))
		{
			if(it->second)
				return true;
			else
				return false;
		}
	}

	PlayerList::iterator playerIt = playerList.find(player->getGUID());
	if(playerIt != playerList.end())
		return true;

	GuildList::iterator guildIt = guildList.find(player->getGuildId());
	if(guildIt != guildList.end())
		return true;

	return false;
}

void AccessList::getList(std::string& _list) const
{
	_list = list;
}

Door::Door(uint16_t _type):
Item(_type)
{
	house = NULL;
	accessList = NULL;
}

Door::~Door()
{
	delete accessList;
}

Attr_ReadValue Door::readAttr(AttrTypes_t attr, PropStream& propStream)
{
	if(ATTR_HOUSEDOORID == attr)
	{
		unsigned char _doorId = 0;
		if(!propStream.GET_UCHAR(_doorId))
			return ATTR_READ_ERROR;

		setDoorId(_doorId);
		return ATTR_READ_CONTINUE;
	}
	else
		return Item::readAttr(attr, propStream);
}

bool Door::serializeAttr(PropWriteStream& propWriteStream) const
{
	return true;
}

void Door::setHouse(House* _house)
{
	if(house != NULL)
	{
		#ifdef __DEBUG_HOUSES__
		std::cout << "Warning: [Door::setHouse] house != NULL" << std::endl;
		#endif
		return;
	}
	house = _house;

	if(!accessList)
		accessList = new AccessList();
}

bool Door::canUse(const Player* player)
{
	if(!house)
		return true;

	if(house->getHouseAccessLevel(player) >= HOUSE_SUBOWNER)
		return true;

	return accessList->isInList(player);
}

void Door::setAccessList(const std::string& textlist)
{
	if(!accessList)
		accessList = new AccessList();

	accessList->parseList(textlist);
}

bool Door::getAccessList(std::string& list) const
{
	if(!house)
	{
		#ifdef __DEBUG_HOUSES__
		std::cout << "Failure: [Door::getAccessList] house == NULL" << std::endl;
		#endif
		return false;
	}
	accessList->getList(list);
	return true;
}

void Door::copyAttributes(Item* item)
{
	Item::copyAttributes(item);

	if(Door* door = item->getDoor())
	{
		std::string list;
		if(door->getAccessList(list))
			setAccessList(list);
	}
}

void Door::onRemoved()
{
	Item::onRemoved();
	if(house)
		house->removeDoor(this);
}

Houses::Houses()
{
	rentPeriod = RENTPERIOD_NEVER;
	std::string strRentPeriod = asLowerCaseString(g_config.getString(ConfigManager::HOUSE_RENT_PERIOD));
	if(strRentPeriod == "yearly")
		rentPeriod = RENTPERIOD_YEARLY;
	else if(strRentPeriod == "weekly")
		rentPeriod = RENTPERIOD_WEEKLY;
	else if(strRentPeriod == "monthly")
		rentPeriod = RENTPERIOD_MONTHLY;
	else if(strRentPeriod == "daily")
		rentPeriod = RENTPERIOD_DAILY;
}

Houses::~Houses()
{
	//
}

House* Houses::getHouseByPlayerId(uint32_t playerId)
{
	for(HouseMap::iterator it = houseMap.begin(); it != houseMap.end(); ++it)
	{
		House* house = it->second;
		if(house->getHouseOwner() == playerId)
			return house;
	}
	return NULL;
}

bool Houses::loadHousesXML(std::string filename)
{
	xmlDocPtr doc = xmlParseFile(filename.c_str());
	if(doc)
	{
		xmlNodePtr root, houseNode;
		root = xmlDocGetRootElement(doc);

		if(xmlStrcmp(root->name,(const xmlChar*)"houses") != 0)
		{
			xmlFreeDoc(doc);
			return false;
		}

		int32_t intValue;
		std::string strValue;

		houseNode = root->children;
		while(houseNode)
		{
			if(xmlStrcmp(houseNode->name,(const xmlChar*)"house") == 0)
			{
				int32_t _houseid = 0;
				Position entryPos(0, 0, 0);

				if(!readXMLInteger(houseNode, "houseid", _houseid))
				{
					xmlFreeDoc(doc);
					return false;
				}

				House* house = Houses::getInstance().getHouse(_houseid);
				if(!house)
				{
					std::cout << "Error: [Houses::loadHousesXML] Unknown house, id = " << _houseid << std::endl;
					xmlFreeDoc(doc);
					return false;
				}

				if(readXMLString(houseNode, "name", strValue))
					house->setName(strValue);

				if(readXMLInteger(houseNode, "entryx", intValue))
					entryPos.x = intValue;

				if(readXMLInteger(houseNode, "entryy", intValue))
					entryPos.y = intValue;

				if(readXMLInteger(houseNode, "entryz", intValue))
					entryPos.z = intValue;

				if(entryPos.x == 0 && entryPos.y == 0 && entryPos.z == 0)
				{
					std::cout << "Warning: [Houses::loadHousesXML] House entry not set"
						<< " - Name: " << house->getName()
						<< " - House id: " << _houseid << std::endl;
				}

				house->setEntryPos(entryPos);

				if(readXMLInteger(houseNode, "rent", intValue))
					house->setRent(intValue);

				if(readXMLInteger(houseNode, "townid", intValue))
					house->setTownId(intValue);

				house->setHouseOwner(0);
			}
			houseNode = houseNode->next;
		}
		xmlFreeDoc(doc);
		return true;
	}
	return false;
}

bool Houses::payHouses()
{
	if(rentPeriod == RENTPERIOD_NEVER)
		return true;

	uint32_t currentTime = time(NULL);
	for(HouseMap::iterator it = houseMap.begin(); it != houseMap.end(); ++it)
	{
		House* house = it->second;
		if(house->getHouseOwner() != 0)
		{
			uint32_t ownerid = house->getHouseOwner();
			Town* town = Towns::getInstance().getTown(house->getTownId());
			if(!town)
			{
				#ifdef __DEBUG_HOUSES__
				std::cout << "Warning: [Houses::payHouses] town = NULL, townid = " <<
					house->getTownId() << ", houseid = " << house->getHouseId() << std::endl;
				#endif
				continue;
			}

			std::string name;
			if(!IOLoginData::getInstance()->getNameByGuid(ownerid, name))
			{
				//player doesnt exist, remove it as house owner?
				house->setHouseOwner(0);
				continue;
			}

			Player* player = g_game.getPlayerByName(name);
			if(!player)
			{
				player = new Player(name, NULL);
				if(!IOLoginData::getInstance()->loadPlayer(player, name))
				{
					#ifdef __DEBUG__
					std::cout << "Failure: [Houses::payHouses], can not load player: " << name << std::endl;
					#endif
					delete player;
					continue;
				}
			}

			int32_t housePrice = 0;
			for(HouseTileList::iterator it = house->getHouseTileBegin(), end = house->getHouseTileEnd(); it != end; ++it)
				housePrice += g_config.getNumber(ConfigManager::HOUSE_PRICE);

			Depot* depot = player->getDepot(town->getTownID(), true);
			if(depot)
			{
				bool paid = false;
				if(player->getBankBalance() >= house->getRent())
				{
					player->setBankBalance(player->getBankBalance() - house->getRent());
					paid = true;
				}

				if(paid)
				{
					time_t paidUntil = currentTime;
					switch(rentPeriod)
					{
						case RENTPERIOD_DAILY:
							paidUntil += 24 * 60 * 60;
							break;
						case RENTPERIOD_WEEKLY:
							paidUntil += 24 * 60 * 60 * 7;
							break;
						case RENTPERIOD_MONTHLY:
							paidUntil += 24 * 60 * 60 * 30;
							break;
						case RENTPERIOD_YEARLY:
							paidUntil += 24 * 60 * 60 * 365;
							break;
						default:
							break;
					}
					house->setPaidUntil(paidUntil);
				}
				else
				{
					player->setDepotChange(true);
					if(house->getPayRentWarnings() < 7)
					{
						int32_t daysLeft = 7 - house->getPayRentWarnings();

						Item* letter = Item::CreateItem(ITEM_LETTER_STAMPED);
						std::string period = "";

						switch(rentPeriod)
						{
							case RENTPERIOD_DAILY:
								period = "daily";
								break;

							case RENTPERIOD_WEEKLY:
								period = "weekly";
								break;

							case RENTPERIOD_MONTHLY:
								period = "monthly";
								break;

							case RENTPERIOD_YEARLY:
								period = "annual";
								break;

							default:
								break;
						}

						std::ostringstream ss;
						ss << "Warning! \nThe " << period << " rent of " << house->getRent() << " gold for your house \"" << house->getName() << "\" is payable. Have it within " << daysLeft << " days or you will lose this house.";
						letter->setText(ss.str());
						g_game.internalAddItem(depot->getInbox(), letter, INDEX_WHEREEVER, FLAG_NOLIMIT);
						house->setPayRentWarnings(house->getPayRentWarnings() + 1);
					}
					else
						house->setHouseOwner(0);
				}
			}

			if(player->isOffline())
			{
				IOLoginData::getInstance()->savePlayer(player, true);
				delete player;
			}
		}
	}
	return true;
}
