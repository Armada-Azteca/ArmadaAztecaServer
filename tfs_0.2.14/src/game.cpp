//////////////////////////////////////////////////////////////////////
// OpenTibia - an opensource roleplaying game
//////////////////////////////////////////////////////////////////////
// class representing the gamestate
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

#include "definitions.h"

#include <string>
#include <map>
#include <fstream>

#include <boost/config.hpp>
#include <boost/bind.hpp>

#include "otsystem.h"
#include "tasks.h"
#include "items.h"
#include "commands.h"
#include "creature.h"
#include "player.h"
#include "monster.h"
#include "game.h"
#include "tile.h"
#include "house.h"
#include "actions.h"
#include "combat.h"
#include "iologindata.h"
#include "iomarket.h"
#include "chat.h"
#include "luascript.h"
#include "talkaction.h"
#include "spells.h"
#include "configmanager.h"
#include "ban.h"
#include "raids.h"
#include "database.h"
#include "server.h"
#include "ioguild.h"
#include "quests.h"
#include "globalevent.h"
#include "mounts.h"

extern ConfigManager g_config;
extern Actions* g_actions;
extern Commands commands;
extern Chat g_chat;
extern TalkActions* g_talkActions;
extern Spells* g_spells;
extern Vocations g_vocations;
extern GlobalEvents* g_globalEvents;

Game::Game()
{
	gameState = GAME_STATE_NORMAL;
	worldType = WORLD_TYPE_PVP;

	checkLightEvent = 0;
	checkCreatureEvent = 0;
	checkDecayEvent = 0;

	map = NULL;
	lastStageLevel = 0;
	lastPlayersRecord = 0;
	useLastStageLevel = false;
	stagesEnabled = false;
	stateTime = OTSYS_TIME();
	for(int16_t i = 0; i < 3; i++)
		serverSaveMessage[i] = false;

	OTSYS_THREAD_LOCKVARINIT(AutoID::autoIDLock);

	#ifdef __EXCEPTION_TRACER__
	OTSYS_THREAD_LOCKVARINIT(maploadlock);
	#endif

	lastBucket = 0;

	//(1440 minutes/day)/(3600 seconds/day)*10 seconds event interval
	int32_t dayCycle = 3600;
	lightHourDelta = 1440 * 10 / dayCycle;
	lightHour = SUNRISE + (SUNSET - SUNRISE) / 2;
	lightLevel = LIGHT_LEVEL_DAY;
	lightState = LIGHT_STATE_DAY;
}

Game::~Game()
{
	blacklist.clear();
	whitelist.clear();

	delete map;

	g_scheduler.stopEvent(checkLightEvent);
	g_scheduler.stopEvent(checkCreatureEvent);
	g_scheduler.stopEvent(checkDecayEvent);
}

void Game::start(ServiceManager* servicer)
{
	services = servicer;

	checkLightEvent = g_scheduler.addEvent(createSchedulerTask(EVENT_LIGHTINTERVAL,
		boost::bind(&Game::checkLight, this)));

	checkCreatureLastIndex = 0;
	checkCreatureEvent = g_scheduler.addEvent(createSchedulerTask(EVENT_CREATURE_THINK_INTERVAL,
		boost::bind(&Game::checkCreatures, this)));

	checkDecayEvent = g_scheduler.addEvent(createSchedulerTask(EVENT_DECAYINTERVAL,
		boost::bind(&Game::checkDecay, this)));
}

GameState_t Game::getGameState() const
{
	return gameState;
}

void Game::setWorldType(WorldType_t type)
{
	worldType = type;
}

void Game::setGameState(GameState_t newState)
{
	if(gameState == GAME_STATE_SHUTDOWN)
		return; //this cannot be stopped

	if(gameState != newState)
	{
		gameState = newState;

		switch(newState)
		{
			case GAME_STATE_INIT:
			{
				Spawns::getInstance()->startup();

				Raids::getInstance()->loadFromXml();
				Raids::getInstance()->startup();

				Quests::getInstance()->loadFromXml();

				Mounts::getInstance()->loadFromXml();

				loadStatuslist();
				loadMotd();
				loadPlayersRecord();

				loadGameState();
				g_globalEvents->startup();
				break;
			}

			case GAME_STATE_SHUTDOWN:
			{
				g_globalEvents->execute(GLOBALEVENT_SHUTDOWN);
				//kick all players that are still online
				AutoList<Player>::listiterator it = Player::listPlayer.list.begin();
				while(it != Player::listPlayer.list.end())
				{
					(*it).second->kickPlayer(true);
					it = Player::listPlayer.list.begin();
				}

				saveGameState();

				g_dispatcher.addTask(
					createTask(boost::bind(&Game::shutdown, this)));

				g_scheduler.stop();
				g_dispatcher.stop();
				break;
			}

			case GAME_STATE_CLOSED:
			{
				/* kick all players without the CanAlwaysLogin flag */
				AutoList<Player>::listiterator it = Player::listPlayer.list.begin();
				while(it != Player::listPlayer.list.end())
				{
					if(!(*it).second->hasFlag(PlayerFlag_CanAlwaysLogin))
					{
						(*it).second->kickPlayer(true);
						it = Player::listPlayer.list.begin();
					}
					else
						++it;
				}

				Houses::getInstance().payHouses();
				saveGameState();
				break;
			}

			default:
				break;
		}
	}
}

void Game::saveGameState()
{
	if(gameState == GAME_STATE_NORMAL)
		setGameState(GAME_STATE_MAINTAIN);

	stateTime = 0;
	std::cout << "Saving server..." << std::endl;
	IOLoginData* ioLoginData = IOLoginData::getInstance();
	for(AutoList<Player>::listiterator it = Player::listPlayer.list.begin(); it != Player::listPlayer.list.end(); ++it)
	{
		(*it).second->loginPosition = (*it).second->getPosition();
		ioLoginData->savePlayer((*it).second, false);
	}

	map->saveMap();
	ScriptEnvironment::saveGameState();
	stateTime = OTSYS_TIME() + STATE_TIME;

	if(gameState == GAME_STATE_MAINTAIN)
		setGameState(GAME_STATE_NORMAL);
}

void Game::loadGameState()
{
	ScriptEnvironment::loadGameState();
}

int32_t Game::loadMap(std::string filename)
{
	if(!map)
		map = new Map;

	maxPlayers = g_config.getNumber(ConfigManager::MAX_PLAYERS);
	inFightTicks = g_config.getNumber(ConfigManager::PZ_LOCKED);
	Player::maxMessageBuffer = g_config.getNumber(ConfigManager::MAX_MESSAGEBUFFER);
	Monster::despawnRange = g_config.getNumber(ConfigManager::DEFAULT_DESPAWNRANGE);
	Monster::despawnRadius = g_config.getNumber(ConfigManager::DEFAULT_DESPAWNRADIUS);
	return map->loadMap("data/world/" + filename + ".otbm");
}

void Game::refreshMap()
{
	Tile* tile;
	Item* item;

	for(Map::TileMap::iterator it = map->refreshTileMap.begin(); it != map->refreshTileMap.end(); ++it)
	{
		tile = it->first;

		if(TileItemVector* items = tile->getItemList())
		{
			//remove garbage
			int32_t downItemSize = tile->getDownItemCount();
			for(int32_t i = downItemSize - 1; i >= 0; --i)
			{
				item = items->at(i);
				if(item)
				{
					#ifndef __DEBUG__
					internalRemoveItem(item);
					#else
					ReturnValue ret = internalRemoveItem(item);
					if(ret != RET_NOERROR)
						std::cout << "Could not refresh item: " << item->getID() << "pos: " << tile->getPosition() << std::endl;
					#endif
				}
			}
		}

		cleanup();

		//restore to original state
		TileItemVector list = it->second.list;
		for(ItemVector::reverse_iterator it = list.rbegin(); it != list.rend(); ++it)
		{
			Item* item = (*it)->clone();
			ReturnValue ret = internalAddItem(tile, item, INDEX_WHEREEVER, FLAG_NOLIMIT);
			if(ret == RET_NOERROR)
			{
				if(item->getUniqueId() != 0)
					ScriptEnvironment::addUniqueThing(item);

				startDecay(item);
			}
			else
			{
				std::cout << "Could not refresh item: " << item->getID() << "pos: " << tile->getPosition() << std::endl;
				delete item;
			}
		}
	}
}

Cylinder* Game::internalGetCylinder(Player* player, const Position& pos)
{
	if(pos.x != 0xFFFF)
		return getTile(pos.x, pos.y, pos.z);
	else
	{
		//container
		if(pos.y & 0x40)
		{
			uint8_t from_cid = pos.y & 0x0F;
			return player->getContainer(from_cid);
		}
		//inventory
		else
			return player;
	}
}

Thing* Game::internalGetThing(Player* player, const Position& pos, int32_t index, uint32_t spriteId /*= 0*/, stackPosType_t type /*= STACKPOS_NORMAL*/)
{
	if(pos.x != 0xFFFF)
	{
		Tile* tile = getTile(pos.x, pos.y, pos.z);
		if(tile)
		{
			/*look at*/
			if(type == STACKPOS_LOOK)
				return tile->getTopVisibleThing(player);

			Thing* thing = NULL;
			/*for move operations*/
			if(type == STACKPOS_MOVE)
			{
				Item* item = tile->getTopDownItem();
				if(item && !item->isNotMoveable())
					thing = item;
				else
					thing = tile->getTopVisibleCreature(player);
			}
			else if(type == STACKPOS_USEITEM)
			{
				//First check items with topOrder 2 (ladders, signs, splashes)
				Item* item = tile->getItemByTopOrder(2);
				if(item && g_actions->hasAction(item))
					thing = item;
				else
				{
					//then down items
					thing = tile->getTopDownItem();
					if(thing == NULL)
						thing = tile->getTopTopItem(); //then last we check items with topOrder 3 (doors etc)

					if(thing == NULL)
						thing = tile->ground;
				}
			}
			else if(type == STACKPOS_USE)
				thing = tile->getTopDownItem();
			else
				thing = tile->__getThing(index);

			if(player)
			{
				//do extra checks here if the thing is accessable
				if(thing && thing->getItem())
				{
					if(tile->hasProperty(ISVERTICAL))
					{
						if(player->getPosition().x + 1 == tile->getPosition().x)
							thing = NULL;
					}
					else if(tile->hasProperty(ISHORIZONTAL))
					{
						if(player->getPosition().y + 1 == tile->getPosition().y)
							thing = NULL;
					}
				}
			}
			return thing;
		}
	}
	else
	{
		//container
		if(pos.y & 0x40)
		{
			uint8_t fromCid = pos.y & 0x0F;
			uint8_t slot = pos.z;

			Container* parentcontainer = player->getContainer(fromCid);
			if(!parentcontainer)
				return NULL;

			return parentcontainer->getItem(slot);
		}
		else if(pos.y == 0 && pos.z == 0)
		{
			const ItemType& it = Item::items.getItemIdByClientId(spriteId);
			if(it.id == 0)
				return NULL;

			int32_t subType = -1;
			if(it.isFluidContainer() && index < int32_t(sizeof(reverseFluidMap) / sizeof(int8_t)))
				subType = reverseFluidMap[index];

			return findItemOfType(player, it.id, true, subType);
		}
		//inventory
		else
		{
			slots_t slot = (slots_t)static_cast<unsigned char>(pos.y);
			return player->getInventoryItem(slot);
		}
	}
	return NULL;
}

void Game::internalGetPosition(Item* item, Position& pos, uint8_t& stackpos)
{
	pos.x = 0;
	pos.y = 0;
	pos.z = 0;
	stackpos = 0;

	Cylinder* topParent = item->getTopParent();
	if(topParent)
	{
		if(Player* player = dynamic_cast<Player*>(topParent))
		{
			pos.x = 0xFFFF;

			Container* container = dynamic_cast<Container*>(item->getParent());
			if(container)
			{
				pos.y = ((uint16_t) ((uint16_t)0x40) | ((uint16_t)player->getContainerID(container)) );
				pos.z = container->__getIndexOfThing(item);
				stackpos = pos.z;
			}
			else
			{
				pos.y = player->__getIndexOfThing(item);
				stackpos = pos.y;
			}
		}
		else if(Tile* tile = topParent->getTile())
		{
			pos = tile->getPosition();
			stackpos = tile->__getIndexOfThing(item);
		}
	}
}

void Game::setTile(Tile* newTile)
{
	return map->setTile(newTile->getPosition(), newTile);
}

Tile* Game::getTile(int32_t x, int32_t y, int32_t z)
{
	return map->getTile(x, y, z);
}

Tile* Game::getTile(const Position& pos)
{
	return map->getTile(pos);
}

QTreeLeafNode* Game::getLeaf(uint32_t x, uint32_t y)
{
	return map->getLeaf(x, y);
}

Creature* Game::getCreatureByID(uint32_t id)
{
	if(id == 0)
		return NULL;

	AutoList<Creature>::listiterator it = listCreature.list.find(id);
	if(it != listCreature.list.end())
	{
		if(!(*it).second->isRemoved())
			return (*it).second;
	}
	return NULL; //just in case the player doesnt exist
}

Player* Game::getPlayerByID(uint32_t id)
{
	if(id == 0)
		return NULL;

	AutoList<Player>::listiterator it = Player::listPlayer.list.find(id);
	if(it != Player::listPlayer.list.end())
	{
		if(!(*it).second->isRemoved())
			return (*it).second;
	}
	return NULL; //just in case the player doesnt exist
}

Creature* Game::getCreatureByName(const std::string& s)
{
	if(s.empty())
		return NULL;

	std::string txt1 = asUpperCaseString(s);
	for(AutoList<Creature>::listiterator it = listCreature.list.begin(); it != listCreature.list.end(); ++it)
	{
		if(!(*it).second->isRemoved())
		{
			std::string txt2 = asUpperCaseString((*it).second->getName());
			if(txt1 == txt2)
				return it->second;
		}
	}
	return NULL; //just in case the creature doesnt exist
}

Player* Game::getPlayerByName(const std::string& s)
{
	if(s.empty())
		return NULL;

	std::string txt1 = asUpperCaseString(s);
	for(AutoList<Player>::listiterator it = Player::listPlayer.list.begin(); it != Player::listPlayer.list.end(); ++it)
	{
		if(!(*it).second->isRemoved())
		{
			std::string txt2 = asUpperCaseString((*it).second->getName());
			if(txt1 == txt2)
				return it->second;
		}
	}
	return NULL; //just in case the player doesnt exist
}

Player* Game::getPlayerByGUID(const uint32_t& guid)
{
	if(guid == 0)
		return NULL;

	for(AutoList<Player>::listiterator it = Player::listPlayer.list.begin(); it != Player::listPlayer.list.end(); ++it)
	{
		Player* player = (*it).second;
		if(!player->isRemoved())
		{
			if(guid == player->getGUID())
				return player;
		}
	}
	return NULL;
}

ReturnValue Game::getPlayerByNameWildcard(const std::string& s, Player*& player)
{
	player = NULL;

	if(s.empty())
		return RET_PLAYERWITHTHISNAMEISNOTONLINE;

	if((*s.rbegin()) != '~')
	{
		player = getPlayerByName(s);
		if(!player)
			return RET_PLAYERWITHTHISNAMEISNOTONLINE;

		return RET_NOERROR;
	}

	Player* lastFound = NULL;
	std::string txt1 = asUpperCaseString(s.substr(0, s.length() - 1));
	for(AutoList<Player>::listiterator it = Player::listPlayer.list.begin(); it != Player::listPlayer.list.end(); ++it)
	{
		if(!(*it).second->isRemoved())
		{
			std::string txt2 = asUpperCaseString((*it).second->getName());
			if(txt2.substr(0, txt1.length()) == txt1)
			{
				if(lastFound == NULL)
					lastFound = (*it).second;
				else
					return RET_NAMEISTOOAMBIGIOUS;
			}
		}
	}

	if(lastFound != NULL)
	{
		player = lastFound;
		return RET_NOERROR;
	}
	return RET_PLAYERWITHTHISNAMEISNOTONLINE;
}

Player* Game::getPlayerByAccount(uint32_t acc)
{
	for(AutoList<Player>::listiterator it = Player::listPlayer.list.begin(); it != Player::listPlayer.list.end(); ++it)
	{
		if(!it->second->isRemoved())
		{
			if(it->second->getAccount() == acc)
				return it->second;
		}
	}
	return NULL;
}

PlayerVector Game::getPlayersByAccount(uint32_t acc)
{
	PlayerVector players;
	for(AutoList<Player>::listiterator it = Player::listPlayer.list.begin(); it != Player::listPlayer.list.end(); ++it)
	{
		if(!it->second->isRemoved())
		{
			if(it->second->getAccount() == acc)
				players.push_back(it->second);
		}
	}
	return players;
}

PlayerVector Game::getPlayersByIP(uint32_t ipadress, uint32_t mask)
{
	PlayerVector players;
	for(AutoList<Player>::listiterator it = Player::listPlayer.list.begin(); it != Player::listPlayer.list.end(); ++it)
	{
		if(!it->second->isRemoved())
		{
			if((it->second->getIP() & mask) == (ipadress & mask))
				players.push_back(it->second);
		}
	}
	return players;
}

bool Game::internalPlaceCreature(Creature* creature, const Position& pos, bool extendedPos /*=false*/, bool forced /*= false*/)
{
	if(creature->getParent() != NULL)
		return false;

	if(!map->placeCreature(pos, creature, extendedPos, forced))
		return false;

	creature->useThing2();
	creature->setID();
	listCreature.addList(creature);
	creature->addList();
	return true;
}

bool Game::placeCreature(Creature* creature, const Position& pos, bool extendedPos /*=false*/, bool forced /*= false*/)
{
	if(!internalPlaceCreature(creature, pos, extendedPos, forced))
		return false;

	SpectatorVec list;
	SpectatorVec::iterator it;
	getSpectators(list, creature->getPosition(), false, true);

	Player* tmpPlayer = NULL;
	for(it = list.begin(); it != list.end(); ++it)
	{
		if((tmpPlayer = (*it)->getPlayer()))
			tmpPlayer->sendCreatureAppear(creature, creature->getPosition(), true);
	}

	for(it = list.begin(); it != list.end(); ++it)
		(*it)->onCreatureAppear(creature, true);

	Cylinder* creatureParent = creature->getParent();
	int32_t newIndex = creatureParent->__getIndexOfThing(creature);
	creatureParent->postAddNotification(creature, NULL, newIndex);

	Player* player = creature->getPlayer();
	if(player)
	{
		int32_t offlineTime;
		if(player->getLastLogout() != 0)
			offlineTime = time(NULL) - player->getLastLogout();
		else
			offlineTime = 0;

		Condition* conditionMuted = player->getCondition(CONDITION_MUTED, CONDITIONID_DEFAULT);
		if(conditionMuted && conditionMuted->getTicks() > 0)
		{
			conditionMuted->setTicks(conditionMuted->getTicks() - (offlineTime * 1000));
			if(conditionMuted->getTicks() <= 0)
				player->removeCondition(conditionMuted);
			else
				player->addCondition(conditionMuted->clone());
		}

		Condition* conditionTrade = player->getCondition(CONDITION_ADVERTISINGTICKS, CONDITIONID_DEFAULT);
		if(conditionTrade && conditionTrade->getTicks() > 0)
		{
			conditionTrade->setTicks(conditionTrade->getTicks() - (offlineTime * 1000));
			if(conditionTrade->getTicks() <= 0)
				player->removeCondition(conditionTrade);
			else
				player->addCondition(conditionTrade->clone());
		}

		Condition* conditionYell = player->getCondition(CONDITION_YELLTICKS, CONDITIONID_DEFAULT);
		if(conditionYell && conditionYell->getTicks() > 0)
		{
			conditionYell->setTicks(conditionYell->getTicks() - (offlineTime * 1000));
			if(conditionYell->getTicks() <= 0)
				player->removeCondition(conditionYell);
			else
				player->addCondition(conditionYell->clone());
		}

		int32_t offlineTrainingSkill = player->getOfflineTrainingSkill();
		if(offlineTrainingSkill == -1)
		{
			uint16_t oldMinutes = player->getOfflineTrainingTime() / 60 / 1000;
			player->addOfflineTrainingTime(offlineTime * 1000);

			uint16_t newMinutes = player->getOfflineTrainingTime() / 60 / 1000;
			if(oldMinutes != newMinutes)
				player->sendStats();
		}

		if(player->isPremium())
		{
			int32_t value;
			player->getStorageValue(STORAGEVALUE_PROMOTION, value);
			if(player->isPromoted() && value != 1)
				player->addStorageValue(STORAGEVALUE_PROMOTION, 1);
			else if(!player->isPromoted() && value == 1)
				player->setVocation(g_vocations.getPromotedVocation(player->getVocationId()));
		}
		else if(player->isPromoted())
			player->setVocation(player->vocation->getFromVocation());
	}

	addCreatureCheck(creature);
	creature->onPlacedCreature();
	return true;
}

bool Game::removeCreature(Creature* creature, bool isLogout /*= true*/)
{
	if(creature->isRemoved())
		return false;

#ifdef __DEBUG__
	std::cout << "removing creature " << std::endl;
#endif

	Tile* tile = creature->getTile();

	SpectatorVec list;
	SpectatorVec::iterator it;
	getSpectators(list, tile->getPosition(), false, true);

	Player* player = NULL;
	std::vector<uint32_t> oldStackPosVector;
	for(it = list.begin(); it != list.end(); ++it)
	{
		if((player = (*it)->getPlayer()))
		{
			if(!creature->isInGhostMode() || player->isAccessPlayer())
				oldStackPosVector.push_back(tile->getClientIndexOfThing(player, creature));
		}
	}

	int32_t index = tile->__getIndexOfThing(creature);
	if(!map->removeCreature(creature))
		return false;

	//send to client
	uint32_t i = 0;
	for(it = list.begin(); it != list.end(); ++it)
	{
		if((player = (*it)->getPlayer()))
		{
			if(!creature->isInGhostMode() || player->isAccessPlayer())
			{
				player->sendCreatureDisappear(creature, oldStackPosVector[i], isLogout);
				++i;
			}
		}
	}

	//event method
	for(it = list.begin(); it != list.end(); ++it)
		(*it)->onCreatureDisappear(creature, index, isLogout);

	creature->getParent()->postRemoveNotification(creature, NULL, index, true);

	listCreature.removeList(creature->getID());
	creature->removeList();
	creature->setRemoved();
	FreeThing(creature);

	removeCreatureCheck(creature);

	for(std::list<Creature*>::iterator cit = creature->summons.begin(); cit != creature->summons.end(); ++cit)
	{
		(*cit)->setLossSkill(false);
		removeCreature(*cit);
	}

	creature->onRemovedCreature();
	return true;
}

bool Game::playerMoveThing(uint32_t playerId, const Position& fromPos,
	uint16_t spriteId, uint8_t fromStackPos, const Position& toPos, uint8_t count)
{
	Player* player = getPlayerByID(playerId);
	if(!player || player->isRemoved())
		return false;

	uint8_t fromIndex = 0;
	if(fromPos.x == 0xFFFF)
	{
		if(fromPos.y & 0x40)
			fromIndex = static_cast<uint8_t>(fromPos.z);
		else
			fromIndex = static_cast<uint8_t>(fromPos.y);
	}
	else
		fromIndex = fromStackPos;

	Thing* thing = internalGetThing(player, fromPos, fromIndex, spriteId, STACKPOS_MOVE);

	Cylinder* toCylinder = internalGetCylinder(player, toPos);

	if(!thing || !toCylinder)
	{
		player->sendCancelMessage(RET_NOTPOSSIBLE);
		return false;
	}

	if(Creature* movingCreature = thing->getCreature())
	{
		if(Position::areInRange<1,1,0>(movingCreature->getPosition(), player->getPosition()))
		{
			SchedulerTask* task = createSchedulerTask(1000,
				boost::bind(&Game::playerMoveCreature, this, player->getID(),
				movingCreature->getID(), movingCreature->getPosition(), toCylinder->getPosition()));
			player->setNextActionTask(task);
		}
		else
			playerMoveCreature(playerId, movingCreature->getID(), movingCreature->getPosition(), toCylinder->getPosition());
	}
	else if(thing->getItem())
		playerMoveItem(playerId, fromPos, spriteId, fromStackPos, toPos, count);

	return true;
}

bool Game::playerMoveCreature(uint32_t playerId, uint32_t movingCreatureId,
	const Position& movingCreatureOrigPos, const Position& toPos)
{
	Player* player = getPlayerByID(playerId);
	if(!player || player->isRemoved())
		return false;

	if(!player->canDoAction())
	{
		uint32_t delay = player->getNextActionTime();
		SchedulerTask* task = createSchedulerTask(delay, boost::bind(&Game::playerMoveCreature,
			this, playerId, movingCreatureId, movingCreatureOrigPos, toPos));
		player->setNextActionTask(task);
		return false;
	}

	player->setNextActionTask(NULL);

	Creature* movingCreature = getCreatureByID(movingCreatureId);
	if(!movingCreature || movingCreature->isRemoved())
		return false;

	if(movingCreature->getPlayer())
	{
		if(movingCreature->getPlayer()->getNoMove())
			return false;
	}

	if(!Position::areInRange<1,1,0>(movingCreatureOrigPos, player->getPosition()))
	{
		//need to walk to the creature first before moving it
		std::list<Direction> listDir;
		if(getPathToEx(player, movingCreatureOrigPos, listDir, 0, 1, true, true))
		{
			g_dispatcher.addTask(createTask(boost::bind(&Game::playerAutoWalk,
				this, player->getID(), listDir)));
			SchedulerTask* task = createSchedulerTask(1500, boost::bind(&Game::playerMoveCreature, this,
				playerId, movingCreatureId, movingCreatureOrigPos, toPos));
			player->setNextWalkActionTask(task);
			return true;
		}
		else
		{
			player->sendCancelMessage(RET_THEREISNOWAY);
			return false;
		}
	}

	Tile* toTile = getTile(toPos);
	const Position& movingCreaturePos = movingCreature->getPosition();

	if(!toTile)
	{
		player->sendCancelMessage(RET_NOTPOSSIBLE);
		return false;
	}

	if((!movingCreature->isPushable() && !player->hasFlag(PlayerFlag_CanPushAllCreatures)) ||
		(movingCreature->isInGhostMode() && !player->isAccessPlayer()))
	{
		player->sendCancelMessage(RET_NOTMOVEABLE);
		return false;
	}

	//check throw distance
	if((std::abs(movingCreaturePos.x - toPos.x) > movingCreature->getThrowRange()) || (std::abs(movingCreaturePos.y - toPos.y) > movingCreature->getThrowRange()) || (std::abs(movingCreaturePos.z - toPos.z) * 4 > movingCreature->getThrowRange()))
	{
		player->sendCancelMessage(RET_DESTINATIONOUTOFREACH);
		return false;
	}

	if(player != movingCreature)
	{
		bool toTileIsSafe = false;
		if(toTile->hasFlag(TILESTATE_NOPVPZONE) || toTile->hasFlag(TILESTATE_PROTECTIONZONE))
			toTileIsSafe = true;

		if(toTile->hasProperty(BLOCKPATH))
		{
			player->sendCancelMessage(RET_NOTENOUGHROOM);
			return false;
		}
		else if((movingCreature->getZone() == ZONE_PROTECTION || movingCreature->getZone() == ZONE_NOPVP)
			&& !toTileIsSafe)
		{
			player->sendCancelMessage(RET_NOTPOSSIBLE);
			return false;
		}
		else if(movingCreature->getNpc() && !Spawns::getInstance()->isInZone(movingCreature->masterPos, movingCreature->masterRadius, toPos))
		{
			player->sendCancelMessage(RET_NOTENOUGHROOM);
			return false;
		}
	}

	ReturnValue ret = internalMoveCreature(movingCreature, movingCreature->getTile(), toTile);
	if(ret != RET_NOERROR)
	{
		player->sendCancelMessage(ret);
		return false;
	}
	return true;
}

ReturnValue Game::internalMoveCreature(Creature* creature, Direction direction, uint32_t flags /*= 0*/)
{
	Cylinder* fromTile = creature->getTile();
	Cylinder* toTile = NULL;

	creature->setLastPosition(creature->getPosition());
	const Position& currentPos = creature->getPosition();
	Position destPos = currentPos;
	bool diagonalMovement;
	switch(direction)
	{
		case NORTHWEST:
		case NORTHEAST:
		case SOUTHWEST:
		case SOUTHEAST:
			diagonalMovement = true;
			break;

		default:
			diagonalMovement = false;
			break;
	}

	destPos = getNextPosition(direction, destPos);
	if(creature->getPlayer() && !diagonalMovement)
	{
		//try go up
		if(currentPos.z != 8 && creature->getTile()->hasHeight(3))
		{
			Tile* tmpTile = getTile(currentPos.x, currentPos.y, currentPos.z - 1);
			if(tmpTile == NULL || (tmpTile->ground == NULL && !tmpTile->hasProperty(BLOCKSOLID)))
			{
				tmpTile = getTile(destPos.x, destPos.y, destPos.z - 1);
				if(tmpTile && tmpTile->ground && !tmpTile->hasProperty(BLOCKSOLID))
				{
					flags = flags | FLAG_IGNOREBLOCKITEM | FLAG_IGNOREBLOCKCREATURE;
					if(!tmpTile->floorChange())
						destPos.z--;
				}
			}
		}
		else
		{
			//try go down
			Tile* tmpTile = getTile(destPos);
			if(currentPos.z != 7 && (tmpTile == NULL || (tmpTile->ground == NULL && !tmpTile->hasProperty(BLOCKSOLID))))
			{
				tmpTile = getTile(destPos.x, destPos.y, destPos.z + 1);
				if(tmpTile && tmpTile->hasHeight(3))
				{
					flags = flags | FLAG_IGNOREBLOCKITEM | FLAG_IGNOREBLOCKCREATURE;
					destPos.z++;
				}
			}
		}
	}

	toTile = getTile(destPos);
	ReturnValue ret = RET_NOTPOSSIBLE;
	if(toTile != NULL)
		ret = internalMoveCreature(creature, fromTile, toTile, flags);

	return ret;
}

ReturnValue Game::internalMoveCreature(Creature* creature, Cylinder* fromCylinder, Cylinder* toCylinder, uint32_t flags /*= 0*/)
{
	//check if we can move the creature to the destination
	ReturnValue ret = toCylinder->__queryAdd(0, creature, 1, flags);
	if(ret != RET_NOERROR)
		return ret;

	fromCylinder->getTile()->moveCreature(creature, toCylinder);

	int32_t index = 0;
	Item* toItem = NULL;
	Cylinder* subCylinder = NULL;

	uint32_t n = 0;
	while((subCylinder = toCylinder->__queryDestination(index, creature, &toItem, flags)) != toCylinder)
	{
		toCylinder->getTile()->moveCreature(creature, subCylinder);

		if(creature->getParent() != subCylinder)
		{
			//could happen if a script move the creature
			break;
		}

		toCylinder = subCylinder;
		flags = 0;

		//to prevent infinite loop
		if(++n >= MAP_MAX_LAYERS)
			break;
	}
	return RET_NOERROR;
}

bool Game::playerMoveItem(uint32_t playerId, const Position& fromPos,
	uint16_t spriteId, uint8_t fromStackPos, const Position& toPos, uint8_t count)
{
	Player* player = getPlayerByID(playerId);
	if(!player || player->isRemoved())
		return false;

	if(!player->canDoAction())
	{
		uint32_t delay = player->getNextActionTime();
		SchedulerTask* task = createSchedulerTask(delay, boost::bind(&Game::playerMoveItem, this,
			playerId, fromPos, spriteId, fromStackPos, toPos, count));
		player->setNextActionTask(task);
		return false;
	}

	player->setNextActionTask(NULL);

	Cylinder* fromCylinder = internalGetCylinder(player, fromPos);
	uint8_t fromIndex = 0;

	if(fromPos.x == 0xFFFF)
	{
		if(fromPos.y & 0x40)
			fromIndex = static_cast<uint8_t>(fromPos.z);
		else
			fromIndex = static_cast<uint8_t>(fromPos.y);
	}
	else
		fromIndex = fromStackPos;

	Thing* thing = internalGetThing(player, fromPos, fromIndex, spriteId, STACKPOS_MOVE);
	if(!thing || !thing->getItem())
	{
		player->sendCancelMessage(RET_NOTPOSSIBLE);
		return false;
	}

	Item* item = thing->getItem();

	Cylinder* toCylinder = internalGetCylinder(player, toPos);
	uint8_t toIndex = 0;

	if(toPos.x == 0xFFFF)
	{
		if(toPos.y & 0x40)
			toIndex = static_cast<uint8_t>(toPos.z);
		else
			toIndex = static_cast<uint8_t>(toPos.y);
	}

	if(fromCylinder == NULL || toCylinder == NULL || item == NULL || item->getClientID() != spriteId)
	{
		player->sendCancelMessage(RET_NOTPOSSIBLE);
		return false;
	}

	if(!item->isPushable() || item->getUniqueId() != 0)
	{
		player->sendCancelMessage(RET_NOTMOVEABLE);
		return false;
	}

	const Position& playerPos = player->getPosition();
	const Position& mapFromPos = fromCylinder->getTile()->getPosition();

	const Tile* toCylinderTile = toCylinder->getTile();
	const Position& mapToPos = toCylinderTile->getPosition();

	if(playerPos.z > mapFromPos.z)
	{
		player->sendCancelMessage(RET_FIRSTGOUPSTAIRS);
		return false;
	}

	if(playerPos.z < mapFromPos.z)
	{
		player->sendCancelMessage(RET_FIRSTGODOWNSTAIRS);
		return false;
	}

	if(!Position::areInRange<1,1,0>(playerPos, mapFromPos))
	{
		//need to walk to the item first before using it
		std::list<Direction> listDir;
		if(getPathToEx(player, item->getPosition(), listDir, 0, 1, true, true))
		{
			g_dispatcher.addTask(createTask(boost::bind(&Game::playerAutoWalk,
				this, player->getID(), listDir)));

			SchedulerTask* task = createSchedulerTask(400, boost::bind(&Game::playerMoveItem, this,
				playerId, fromPos, spriteId, fromStackPos, toPos, count));
			player->setNextWalkActionTask(task);
			return true;
		}
		else
		{
			player->sendCancelMessage(RET_THEREISNOWAY);
			return false;
		}
	}

	//hangable item specific code
	if(item->isHangable() && toCylinderTile->hasProperty(SUPPORTHANGABLE))
	{
		//destination supports hangable objects so need to move there first
		if(toCylinderTile->hasProperty(ISVERTICAL))
		{
			if(player->getPosition().x + 1 == mapToPos.x)
			{
				player->sendCancelMessage(RET_NOTPOSSIBLE);
				return false;
			}
		}
		else if(toCylinderTile->hasProperty(ISHORIZONTAL))
		{
			if(player->getPosition().y + 1 == mapToPos.y)
			{
				player->sendCancelMessage(RET_NOTPOSSIBLE);
				return false;
			}
		}

		if(!Position::areInRange<1,1,0>(playerPos, mapToPos))
		{
			Position walkPos = mapToPos;
			if(toCylinderTile->hasProperty(ISVERTICAL))
				walkPos.x -= -1;

			if(toCylinderTile->hasProperty(ISHORIZONTAL))
				walkPos.y -= -1;

			Position itemPos = fromPos;
			uint8_t itemStackPos = fromStackPos;

			if(fromPos.x != 0xFFFF && Position::areInRange<1,1,0>(mapFromPos, player->getPosition())
				&& !Position::areInRange<1,1,0>(mapFromPos, walkPos))
			{
				//need to pickup the item first
				Item* moveItem = NULL;

				ReturnValue ret = internalMoveItem(fromCylinder, player, INDEX_WHEREEVER, item, count, &moveItem);
				if(ret != RET_NOERROR)
				{
					player->sendCancelMessage(ret);
					return false;
				}

				//changing the position since its now in the inventory of the player
				internalGetPosition(moveItem, itemPos, itemStackPos);
			}

			std::list<Direction> listDir;
			if(map->getPathTo(player, walkPos, listDir))
			{
				g_dispatcher.addTask(createTask(boost::bind(&Game::playerAutoWalk,
					this, player->getID(), listDir)));

				SchedulerTask* task = createSchedulerTask(400, boost::bind(&Game::playerMoveItem, this,
					playerId, itemPos, spriteId, itemStackPos, toPos, count));
				player->setNextWalkActionTask(task);
				return true;
			}
			else
			{
				player->sendCancelMessage(RET_THEREISNOWAY);
				return false;
			}
		}
	}

	if((std::abs(playerPos.x - mapToPos.x) > item->getThrowRange()) ||
			(std::abs(playerPos.y - mapToPos.y) > item->getThrowRange()) ||
			(std::abs(mapFromPos.z - mapToPos.z) * 4 > item->getThrowRange()))
	{
		player->sendCancelMessage(RET_DESTINATIONOUTOFREACH);
		return false;
	}

	if(!canThrowObjectTo(mapFromPos, mapToPos))
	{
		player->sendCancelMessage(RET_CANNOTTHROW);
		return false;
	}

	ReturnValue ret = internalMoveItem(fromCylinder, toCylinder, toIndex, item, count, NULL, 0, player);
	if(ret != RET_NOERROR)
	{
		player->sendCancelMessage(ret);
		return false;
	}
	return true;
}

ReturnValue Game::internalMoveItem(Cylinder* fromCylinder, Cylinder* toCylinder, int32_t index,
	Item* item, uint32_t count, Item** _moveItem, uint32_t flags /*= 0*/, Creature* actor/* = NULL*/)
{
	if(!toCylinder)
		return RET_NOTPOSSIBLE;

	Item* toItem = NULL;

	Cylinder* subCylinder;
	int floorN = 0;
	while((subCylinder = toCylinder->__queryDestination(index, item, &toItem, flags)) != toCylinder)
	{
		toCylinder = subCylinder;
		flags = 0;

		//to prevent infinite loop
		if(++floorN >= MAP_MAX_LAYERS)
			break;
	}

	//destination is the same as the source?
	if(item == toItem)
		return RET_NOERROR; //silently ignore move

	//check if we can add this item
	ReturnValue ret = toCylinder->__queryAdd(index, item, count, flags, actor);
	if(ret == RET_NEEDEXCHANGE)
	{
		//check if we can add it to source cylinder
		int32_t fromIndex = fromCylinder->__getIndexOfThing(item);

		ret = fromCylinder->__queryAdd(fromIndex, toItem, toItem->getItemCount(), 0);
		if(ret == RET_NOERROR)
		{
			//check how much we can move
			uint32_t maxExchangeQueryCount = 0;
			ReturnValue retExchangeMaxCount = fromCylinder->__queryMaxCount(INDEX_WHEREEVER, toItem, toItem->getItemCount(), maxExchangeQueryCount, 0);
			if(retExchangeMaxCount != RET_NOERROR && maxExchangeQueryCount == 0)
				return retExchangeMaxCount;

			if(toCylinder->__queryRemove(toItem, toItem->getItemCount(), flags) == RET_NOERROR)
			{
				int32_t oldToItemIndex = toCylinder->__getIndexOfThing(toItem);
				toCylinder->__removeThing(toItem, toItem->getItemCount());
				fromCylinder->__addThing(toItem);

				if(oldToItemIndex != -1)
					toCylinder->postRemoveNotification(toItem, fromCylinder, oldToItemIndex, true);

				int32_t newToItemIndex = fromCylinder->__getIndexOfThing(toItem);
				if(newToItemIndex != -1)
					fromCylinder->postAddNotification(toItem, toCylinder, newToItemIndex);

				ret = toCylinder->__queryAdd(index, item, count, flags);
				toItem = NULL;
			}
		}
	}

	if(ret != RET_NOERROR)
		return ret;

	//check how much we can move
	uint32_t maxQueryCount = 0;
	ReturnValue retMaxCount = toCylinder->__queryMaxCount(index, item, count, maxQueryCount, flags);

	if(retMaxCount != RET_NOERROR && maxQueryCount == 0)
		return retMaxCount;

	uint32_t m = 0;

	if(item->isStackable())
		m = std::min((uint32_t)count, maxQueryCount);
	else
		m = maxQueryCount;

	Item* moveItem = item;

	//check if we can remove this item
	ret = fromCylinder->__queryRemove(item, m, flags);
	if(ret != RET_NOERROR)
		return ret;

	//remove the item
	int32_t itemIndex = fromCylinder->__getIndexOfThing(item);
	Item* updateItem = NULL;
	fromCylinder->__removeThing(item, m);
	bool isCompleteRemoval = item->isRemoved();

	//update item(s)
	if(item->isStackable())
	{
		uint32_t n;
		if(toItem && toItem->getID() == item->getID())
		{
			n = std::min((uint32_t)100 - toItem->getItemCount(), m);
			toCylinder->__updateThing(toItem, toItem->getID(), toItem->getItemCount() + n);
			updateItem = toItem;
		}
		else
			n = 0;

		int32_t count = m - n;
		if(count > 0)
			moveItem = Item::CreateItem(item->getID(), count);
		else
			moveItem = NULL;

		if(item->isRemoved())
			FreeThing(item);
	}

	//add item
	if(moveItem /*m - n > 0*/)
		toCylinder->__addThing(index, moveItem);

	if(itemIndex != -1)
		fromCylinder->postRemoveNotification(item, toCylinder, itemIndex, isCompleteRemoval);

	if(moveItem)
	{
		int32_t moveItemIndex = toCylinder->__getIndexOfThing(moveItem);
		if(moveItemIndex != -1)
			toCylinder->postAddNotification(moveItem, fromCylinder, moveItemIndex);
	}

	if(updateItem)
	{
		int32_t updateItemIndex = toCylinder->__getIndexOfThing(updateItem);
		if(updateItemIndex != -1)
			toCylinder->postAddNotification(updateItem, fromCylinder, updateItemIndex);
	}

	if(_moveItem)
	{
		if(moveItem)
			*_moveItem = moveItem;
		else
			*_moveItem = item;
	}

	//we could not move all, inform the player
	if(item->isStackable() && maxQueryCount < count)
		return retMaxCount;

	return ret;
}

ReturnValue Game::internalAddItem(Cylinder* toCylinder, Item* item, int32_t index /*= INDEX_WHEREEVER*/,
	uint32_t flags/* = 0*/, bool test/* = false*/)
{
	uint32_t remainderCount = 0;
	return internalAddItem(toCylinder, item, index, flags, test, remainderCount);
}

ReturnValue Game::internalAddItem(Cylinder* toCylinder, Item* item, int32_t index,
	uint32_t flags, bool test, uint32_t& remainderCount)
{
	remainderCount = 0;
	if(toCylinder == NULL || item == NULL)
		return RET_NOTPOSSIBLE;

	Cylinder* destCylinder = toCylinder;
	Item* toItem = NULL;
	toCylinder = toCylinder->__queryDestination(index, item, &toItem, flags);

	//check if we can add this item
	ReturnValue ret = toCylinder->__queryAdd(index, item, item->getItemCount(), flags);
	if(ret != RET_NOERROR)
		return ret;

	/*
	Check if we can move add the whole amount, we do this by checking against the original cylinder,
	since the queryDestination can return a cylinder that might only hold a part of the full amount.
	*/
	uint32_t maxQueryCount = 0;
	ret = destCylinder->__queryMaxCount(INDEX_WHEREEVER, item, item->getItemCount(), maxQueryCount, flags);
	if(ret != RET_NOERROR)
		return ret;

	if(test)
		return RET_NOERROR;

	if(item->isStackable() && toItem && toItem->getID() == item->getID())
	{
		uint32_t m = std::min((uint32_t)item->getItemCount(), maxQueryCount);
		uint32_t n = 0;

		if(toItem->getID() == item->getID())
		{
			n = std::min((uint32_t)100 - toItem->getItemCount(), m);
			toCylinder->__updateThing(toItem, toItem->getID(), toItem->getItemCount() + n);
		}

		int32_t count = m - n;
		if(count > 0)
		{
			if(item->getItemCount() != count)
			{
				Item* remainderItem = Item::CreateItem(item->getID(), count);
				if(internalAddItem(destCylinder, remainderItem, INDEX_WHEREEVER, flags, false) != RET_NOERROR)
				{
					FreeThing(remainderItem);
					remainderCount = count;
				}
			}
			else
			{
				toCylinder->__addThing(index, item);

				int32_t itemIndex = toCylinder->__getIndexOfThing(item);
				if(itemIndex != -1)
					toCylinder->postAddNotification(item, NULL, itemIndex);
			}
		}
		else
		{
			//fully merged with toItem, item will be destroyed
			item->onRemoved();
			FreeThing(item);

			int32_t itemIndex = toCylinder->__getIndexOfThing(toItem);
			if(itemIndex != -1)
				toCylinder->postAddNotification(toItem, NULL, itemIndex);
		}
	}
	else
	{
		toCylinder->__addThing(index, item);

		int32_t itemIndex = toCylinder->__getIndexOfThing(item);
		if(itemIndex != -1)
			toCylinder->postAddNotification(item, NULL, itemIndex);
	}
	return RET_NOERROR;
}

ReturnValue Game::internalRemoveItem(Item* item, int32_t count /*= -1*/, bool test /*= false*/, uint32_t flags /*= 0*/)
{
	Cylinder* cylinder = item->getParent();
	if(cylinder == NULL)
		return RET_NOTPOSSIBLE;

	if(count == -1)
		count = item->getItemCount();

	//check if we can remove this item
	ReturnValue ret = cylinder->__queryRemove(item, count, flags | FLAG_IGNORENOTMOVEABLE);
	if(ret != RET_NOERROR)
		return ret;

	if(!item->canRemove())
		return RET_NOTPOSSIBLE;

	if(!test)
	{
		int32_t index = cylinder->__getIndexOfThing(item);

		//remove the item
		cylinder->__removeThing(item, count);
		bool isCompleteRemoval = false;
		if(item->isRemoved())
		{
			isCompleteRemoval = true;
			FreeThing(item);
		}
		cylinder->postRemoveNotification(item, NULL, index, isCompleteRemoval);
	}
	item->onRemoved();
	return RET_NOERROR;
}

ReturnValue Game::internalPlayerAddItem(Player* player, Item* item, bool dropOnMap /*= true*/, slots_t slot /*= SLOT_WHEREEVER*/)
{
	uint32_t remainderCount = 0;
	ReturnValue ret = internalAddItem(player, item, (int32_t)slot, 0, false, remainderCount);
	if(remainderCount > 0)
	{
		Item* remainderItem = Item::CreateItem(item->getID(), remainderCount);
		ReturnValue remaindRet = internalAddItem(player->getTile(), remainderItem, INDEX_WHEREEVER, FLAG_NOLIMIT);
		if(remaindRet != RET_NOERROR)
			FreeThing(remainderItem);
	}

	if(ret != RET_NOERROR && dropOnMap)
		ret = internalAddItem(player->getTile(), item, INDEX_WHEREEVER, FLAG_NOLIMIT);

	return ret;
}

Item* Game::findItemOfType(Cylinder* cylinder, uint16_t itemId,
	bool depthSearch /*= true*/, int32_t subType /*= -1*/)
{
	if(cylinder == NULL)
		return NULL;

	std::list<Container*> listContainer;
	Container* tmpContainer = NULL;
	Thing* thing = NULL;
	Item* item = NULL;

	for(int32_t i = cylinder->__getFirstIndex(); i < cylinder->__getLastIndex();)
	{
		if((thing = cylinder->__getThing(i)) && (item = thing->getItem()))
		{
			if(item->getID() == itemId && (subType == -1 || subType == item->getSubType()))
				return item;
			else
			{
				++i;
				if(depthSearch && (tmpContainer = item->getContainer()))
					listContainer.push_back(tmpContainer);
			}
		}
		else
			++i;
	}

	while(!listContainer.empty())
	{
		Container* container = listContainer.front();
		listContainer.pop_front();
		for(ItemList::const_iterator it = container->getItems(), end = container->getEnd(); it != end; ++it)
		{
			Item* item = *it;
			if(item->getID() == itemId && (subType == -1 || subType == item->getSubType()))
				return item;

			if((tmpContainer = item->getContainer()))
				listContainer.push_back(tmpContainer);
		}
	}
	return NULL;
}

bool Game::removeItemOfType(Cylinder* cylinder, uint16_t itemId, int32_t count, int32_t subType /*= -1*/)
{
	if(cylinder == NULL || ((int32_t)cylinder->__getItemTypeCount(itemId, subType) < count))
		return false;

	if(count <= 0)
		return true;

	std::list<Container*> listContainer;
	Container* tmpContainer = NULL;
	Thing* thing = NULL;
	Item* item = NULL;

	for(int32_t i = cylinder->__getFirstIndex(); i < cylinder->__getLastIndex() && count > 0;)
	{
		if((thing = cylinder->__getThing(i)) && (item = thing->getItem()))
		{
			if(item->getID() == itemId)
			{
				if(item->isStackable())
				{
					if(item->getItemCount() > count)
					{
						internalRemoveItem(item, count);
						count = 0;
					}
					else
					{
						count -= item->getItemCount();
						internalRemoveItem(item);
					}
				}
				else if(subType == -1 || subType == item->getSubType())
				{
					--count;
					internalRemoveItem(item);
				}
				else
					++i;
			}
			else
			{
				++i;
				if((tmpContainer = item->getContainer()))
					listContainer.push_back(tmpContainer);
			}
		}
		else
			++i;
	}

	while(!listContainer.empty() && count > 0)
	{
		Container* container = listContainer.front();
		listContainer.pop_front();
		for(int32_t i = 0; i < (int32_t)container->size() && count > 0;)
		{
			Item* item = container->getItem(i);
			if(item->getID() == itemId)
			{
				if(item->isStackable())
				{
					if(item->getItemCount() > count)
					{
						internalRemoveItem(item, count);
						count = 0;
					}
					else
					{
						count-= item->getItemCount();
						internalRemoveItem(item);
					}
				}
				else if(subType == -1 || subType == item->getSubType())
				{
					--count;
					internalRemoveItem(item);
				}
				else
					++i;
			}
			else
			{
				++i;
				if((tmpContainer = item->getContainer()))
					listContainer.push_back(tmpContainer);
			}
		}
	}
	return (count == 0);
}

uint32_t Game::getMoney(const Cylinder* cylinder)
{
	if(cylinder == NULL)
		return 0;

	std::list<Container*> listContainer;
	ItemList::const_iterator it;
	Container* tmpContainer;

	Thing* thing;
	Item* item;

	uint32_t moneyCount = 0;
	for(int32_t i = cylinder->__getFirstIndex(); i < cylinder->__getLastIndex(); ++i)
	{
		if(!(thing = cylinder->__getThing(i)))
			continue;

		if(!(item = thing->getItem()))
			continue;

		if((tmpContainer = item->getContainer()))
			listContainer.push_back(tmpContainer);
		else if(item->getWorth() != 0)
			moneyCount += item->getWorth();
	}

	while(!listContainer.empty())
	{
		Container* container = listContainer.front();
		listContainer.pop_front();

		for(it = container->getItems(); it != container->getEnd(); ++it)
		{
			Item* item = *it;
			if((tmpContainer = item->getContainer()))
				listContainer.push_back(tmpContainer);
			else if(item->getWorth() != 0)
				moneyCount += item->getWorth();
		}
	}
	return moneyCount;
}

bool Game::removeMoney(Cylinder* cylinder, int32_t money, uint32_t flags /*= 0*/)
{
	if(cylinder == NULL)
		return false;

	if(money <= 0)
		return true;

	std::list<Container*> listContainer;
	Container* tmpContainer = NULL;

	typedef std::multimap<int, Item*, std::less<int> > MoneyMap;
	typedef MoneyMap::value_type moneymap_pair;
	MoneyMap moneyMap;
	Thing* thing;
	Item* item;
	int32_t moneyCount = 0;

	for(int32_t i = cylinder->__getFirstIndex(); i < cylinder->__getLastIndex(); ++i)
	{
		if(!(thing = cylinder->__getThing(i)))
			continue;

		if(!(item = thing->getItem()))
			continue;

		if((tmpContainer = item->getContainer()))
			listContainer.push_back(tmpContainer);
		else if(item->getWorth() != 0)
		{
			moneyCount += item->getWorth();
			moneyMap.insert(moneymap_pair(item->getWorth(), item));
		}
	}

	while(!listContainer.empty())
	{
		Container* container = listContainer.front();
		listContainer.pop_front();

		for(ItemList::const_iterator it = container->getItems(), end = container->getEnd(); it != end; ++it)
		{
			Item* item = *it;
			if((tmpContainer = item->getContainer()))
				listContainer.push_back(tmpContainer);
			else if(item->getWorth() != 0)
			{
				moneyCount += item->getWorth();
				moneyMap.insert(moneymap_pair(item->getWorth(), item));
			}
		}
	}

	/*not enough money*/
	if(moneyCount < money)
		return false;

	MoneyMap::iterator mit, mend;
	for(mit = moneyMap.begin(), mend = moneyMap.end(); mit != mend && money > 0; ++mit)
	{
		Item* item = mit->second;
		internalRemoveItem(item);
		if(mit->first > money)
		{
			/* Remove a monetary value from an item*/
			int32_t remaind = item->getWorth() - money;
			addMoney(cylinder, remaind, flags);
			money = 0;
		}
		else
			money -= mit->first;
	}
	return (money == 0);
}

void Game::addMoney(Cylinder* cylinder, int32_t money, uint32_t flags /*= 0*/)
{
	int32_t crys = money / 10000;
	money -= crys * 10000;
	int32_t plat = money / 100;
	money -= plat * 100;
	int32_t gold = money;

	if(crys != 0)
	{
		do
		{
			Item* remaindItem = Item::CreateItem(ITEM_COINS_CRYSTAL, std::min(100, crys));

			ReturnValue ret = internalAddItem(cylinder, remaindItem, INDEX_WHEREEVER, flags);
			if(ret != RET_NOERROR)
				internalAddItem(cylinder->getTile(), remaindItem, INDEX_WHEREEVER, FLAG_NOLIMIT);

			crys -= std::min(100, crys);
		}
		while(crys > 0);
	}

	if(plat != 0)
	{
		Item* remaindItem = Item::CreateItem(ITEM_COINS_PLATINUM, plat);

		ReturnValue ret = internalAddItem(cylinder, remaindItem, INDEX_WHEREEVER, flags);
		if(ret != RET_NOERROR)
			internalAddItem(cylinder->getTile(), remaindItem, INDEX_WHEREEVER, FLAG_NOLIMIT);
	}

	if(gold != 0)
	{
		Item* remaindItem = Item::CreateItem(ITEM_COINS_GOLD, gold);

		ReturnValue ret = internalAddItem(cylinder, remaindItem, INDEX_WHEREEVER, flags);
		if(ret != RET_NOERROR)
			internalAddItem(cylinder->getTile(), remaindItem, INDEX_WHEREEVER, FLAG_NOLIMIT);
	}
}

Item* Game::transformItem(Item* item, uint16_t newId, int32_t newCount /*= -1*/)
{
	if(item->getID() == newId && (newCount == -1 || (newCount == item->getSubType() && newCount != 0))) //chargeless item placed on map = infinite
		return item;

	Cylinder* cylinder = item->getParent();
	if(cylinder == NULL)
		return NULL;

	int32_t itemIndex = cylinder->__getIndexOfThing(item);
	if(itemIndex == -1)
	{
#ifdef __DEBUG__
		std::cout << "Error: transformItem, itemIndex == -1" << std::endl;
#endif
		return item;
	}

	if(!item->canTransform())
		return item;

	const ItemType& curType = Item::items[item->getID()];
	const ItemType& newType = Item::items[newId];

	if(curType.alwaysOnTop != newType.alwaysOnTop)
	{
		//This only occurs when you transform items on tiles from a downItem to a topItem (or vice versa)
		//Remove the old, and add the new
		ReturnValue ret = internalRemoveItem(item);
		if(ret != RET_NOERROR)
			return item;

		Item* newItem = NULL;
		if(newCount == -1)
			newItem = Item::CreateItem(newId);
		else
			newItem = Item::CreateItem(newId, newCount);

		if(newItem == NULL)
		{
			#ifdef __DEBUG__
			std::cout << "Error: [Game::transformItem] Item of type " << item->getID() << " transforming into invalid type " << newId << std::endl;
			#endif
			return NULL;
		}

		newItem->copyAttributes(item);

		ret = internalAddItem(cylinder, newItem, INDEX_WHEREEVER, FLAG_NOLIMIT);
		if(ret != RET_NOERROR)
		{
			delete newItem;
			return NULL;
		}
		return newItem;
	}

	if(curType.type == newType.type)
	{
		//Both items has the same type so we can safely change id/subtype
		if(newCount == 0 && (item->isStackable() || item->hasCharges()))
		{
			if(item->isStackable())
			{
				internalRemoveItem(item);
				return NULL;
			}
			else
			{
				int32_t newItemId = newId;
				if(curType.id == newType.id)
					newItemId = curType.decayTo;

				if(newItemId == -1)
				{
					internalRemoveItem(item);
					return NULL;
				}
				else if(newItemId != newId)
				{
					//Replacing the the old item with the new while maintaining the old position
					Item* newItem = Item::CreateItem(newItemId, 1);
					if(newItem == NULL)
					{
						#ifdef __DEBUG__
						std::cout << "Error: [Game::transformItem] Item of type " << item->getID() << " transforming into invalid type " << newItemId << std::endl;
						#endif
						return NULL;
					}

					cylinder->__replaceThing(itemIndex, newItem);
					cylinder->postAddNotification(newItem, cylinder, itemIndex);

					item->setParent(NULL);
					cylinder->postRemoveNotification(item, cylinder, itemIndex, true);
					FreeThing(item);
					return newItem;
				}
				else
				{
					item = transformItem(item, newItemId);
					return item;
				}
			}
		}
		else
		{
			cylinder->postRemoveNotification(item, cylinder, itemIndex, false);
			uint16_t itemId = item->getID();
			int32_t count = item->getSubType();

			if(curType.id != newType.id)
			{
				if(newType.group != curType.group)
					item->setDefaultSubtype();

				itemId = newId;
			}

			if(newCount != -1 && newType.hasSubType())
				count = newCount;

			cylinder->__updateThing(item, itemId, count);
			cylinder->postAddNotification(item, cylinder, itemIndex);
			return item;
		}
	}
	else
	{
		//Replacing the the old item with the new while maintaining the old position
		Item* newItem = NULL;
		if(newCount == -1)
			newItem = Item::CreateItem(newId);
		else
			newItem = Item::CreateItem(newId, newCount);

		if(newItem == NULL)
		{
			#ifdef __DEBUG__
			std::cout << "Error: [Game::transformItem] Item of type " << item->getID() << " transforming into invalid type " << newId << std::endl;
			#endif
			return NULL;
		}

		cylinder->__replaceThing(itemIndex, newItem);
		cylinder->postAddNotification(newItem, cylinder, itemIndex);

		item->setParent(NULL);
		cylinder->postRemoveNotification(item, cylinder, itemIndex, true);
		FreeThing(item);

		return newItem;
	}
	return NULL;
}

ReturnValue Game::internalTeleport(Thing* thing, const Position& newPos, bool pushMove/* = true*/, uint32_t flags /*= 0*/)
{
	if(newPos == thing->getPosition())
		return RET_NOERROR;
	else if(thing->isRemoved())
		return RET_NOTPOSSIBLE;

	Tile* toTile = getTile(newPos.x, newPos.y, newPos.z);
	if(toTile)
	{
		if(Creature* creature = thing->getCreature())
		{
			ReturnValue ret = toTile->__queryAdd(0, creature, 1, FLAG_NOLIMIT);
			if(ret != RET_NOERROR)
				return ret;

			creature->getTile()->moveCreature(creature, toTile, !pushMove);
			return RET_NOERROR;
		}
		else if(Item* item = thing->getItem())
			return internalMoveItem(item->getParent(), toTile, INDEX_WHEREEVER, item, item->getItemCount(), NULL, flags);
	}
	return RET_NOTPOSSIBLE;
}

//Implementation of player invoked events
bool Game::playerMove(uint32_t playerId, Direction direction)
{
	Player* player = getPlayerByID(playerId);
	if(!player || player->isRemoved())
		return false;

	player->resetIdleTime();
	player->setNextWalkActionTask(NULL);

	std::list<Direction> dirs;
	dirs.push_back(direction);
	return player->startAutoWalk(dirs);
}

bool Game::playerBroadcastMessage(Player* player, const std::string& text)
{
	if(!player->hasFlag(PlayerFlag_CanBroadcast))
		return false;

	std::cout << "> " << player->getName() << " broadcasted: \"" << text << "\"." << std::endl;
	for(AutoList<Player>::listiterator it = Player::listPlayer.list.begin(); it != Player::listPlayer.list.end(); ++it)
		(*it).second->sendCreatureSay(player, SPEAK_BROADCAST, text);

	return true;
}

bool Game::playerCreatePrivateChannel(uint32_t playerId)
{
	Player* player = getPlayerByID(playerId);
	if(!player || player->isRemoved() || !player->isPremium())
		return false;

	ChatChannel* channel = g_chat.createChannel(player, CHANNEL_PRIVATE);
	if(!channel || !channel->addUser(player))
		return false;

	player->sendCreatePrivateChannel(channel->getId(), channel->getName());
	return true;
}

bool Game::playerChannelInvite(uint32_t playerId, const std::string& name)
{
	Player* player = getPlayerByID(playerId);
	if(!player || player->isRemoved())
		return false;

	PrivateChatChannel* channel = g_chat.getPrivateChannel(player);
	if(!channel)
		return false;

	Player* invitePlayer = getPlayerByName(name);
	if(!invitePlayer)
		return false;

	channel->invitePlayer(player, invitePlayer);
	return true;
}

bool Game::playerChannelExclude(uint32_t playerId, const std::string& name)
{
	Player* player = getPlayerByID(playerId);
	if(!player || player->isRemoved())
		return false;

	PrivateChatChannel* channel = g_chat.getPrivateChannel(player);
	if(!channel)
		return false;

	Player* excludePlayer = getPlayerByName(name);
	if(!excludePlayer)
		return false;

	channel->excludePlayer(player, excludePlayer);
	return true;
}

bool Game::playerRequestChannels(uint32_t playerId)
{
	Player* player = getPlayerByID(playerId);
	if(!player || player->isRemoved())
		return false;

	player->sendChannelsDialog();
	return true;
}

bool Game::playerOpenChannel(uint32_t playerId, uint16_t channelId)
{
	Player* player = getPlayerByID(playerId);
	if(!player || player->isRemoved())
		return false;

	ChatChannel* channel = g_chat.addUserToChannel(player, channelId);
	if(!channel)
		return false;

	player->sendChannel(channel->getId(), channel->getName());
	return true;
}

bool Game::playerCloseChannel(uint32_t playerId, uint16_t channelId)
{
	Player* player = getPlayerByID(playerId);
	if(!player || player->isRemoved())
		return false;

	g_chat.removeUserFromChannel(player, channelId);
	return true;
}

bool Game::playerOpenPrivateChannel(uint32_t playerId, std::string& receiver)
{
	Player* player = getPlayerByID(playerId);
	if(!player || player->isRemoved())
		return false;

	if(!IOLoginData::getInstance()->playerExists(receiver))
	{
		player->sendCancel("A player with this name does not exist.");
		return true;
	}

	player->sendOpenPrivateChannel(receiver);
	return true;
}

bool Game::playerCloseNpcChannel(uint32_t playerId)
{
	Player* player = getPlayerByID(playerId);
	if(!player || player->isRemoved())
		return false;

	SpectatorVec list;
	SpectatorVec::iterator it;
	getSpectators(list, player->getPosition());
	Npc* npc;

	for(it = list.begin(); it != list.end(); ++it)
	{
		if((npc = (*it)->getNpc()))
			npc->onPlayerCloseChannel(player);
	}
	return true;
}

bool Game::playerReceivePing(uint32_t playerId)
{
	Player* player = getPlayerByID(playerId);
	if(!player || player->isRemoved())
		return false;

	player->receivePing();
	return true;
}

bool Game::playerReceivePingBack(uint32_t playerId)
{
	Player* player = getPlayerByID(playerId);
	if(!player || player->isRemoved())
		return false;

	player->sendPingBack();
	return true;
}

bool Game::playerAutoWalk(uint32_t playerId, std::list<Direction>& listDir)
{
	Player* player = getPlayerByID(playerId);
	if(!player || player->isRemoved())
		return false;

	player->resetIdleTime();
	player->setNextWalkTask(NULL);
	return player->startAutoWalk(listDir);
}

bool Game::playerStopAutoWalk(uint32_t playerId)
{
	Player* player = getPlayerByID(playerId);
	if(!player || player->isRemoved())
		return false;

	player->stopWalk();
	return true;
}

bool Game::playerUseItemEx(uint32_t playerId, const Position& fromPos, uint8_t fromStackPos, uint16_t fromSpriteId,
	const Position& toPos, uint8_t toStackPos, uint16_t toSpriteId, bool isHotkey)
{
	Player* player = getPlayerByID(playerId);
	if(!player || player->isRemoved())
		return false;

	if(isHotkey && !g_config.getBoolean(ConfigManager::AIMBOT_HOTKEY_ENABLED))
		return false;

	Thing* thing = internalGetThing(player, fromPos, fromStackPos, fromSpriteId, STACKPOS_USEITEM);
	if(!thing)
	{
		player->sendCancelMessage(RET_NOTPOSSIBLE);
		return false;
	}

	Item* item = thing->getItem();
	if(!item || !item->isUseable())
	{
		player->sendCancelMessage(RET_CANNOTUSETHISOBJECT);
		return false;
	}

	Position walkToPos = fromPos;
	ReturnValue ret = g_actions->canUse(player, fromPos);
	if(ret == RET_NOERROR)
	{
		ret = g_actions->canUse(player, toPos, item);
		if(ret == RET_TOOFARAWAY)
			walkToPos = toPos;
	}

	if(ret != RET_NOERROR)
	{
		if(ret == RET_TOOFARAWAY)
		{
			Position itemPos = fromPos;
			uint8_t itemStackPos = fromStackPos;

			if(fromPos.x != 0xFFFF && toPos.x != 0xFFFF && Position::areInRange<1,1,0>(fromPos, player->getPosition()) &&
				!Position::areInRange<1,1,0>(fromPos, toPos))
			{
				Item* moveItem = NULL;

				ReturnValue ret = internalMoveItem(item->getParent(), player, INDEX_WHEREEVER,
					item, item->getItemCount(), &moveItem);
				if(ret != RET_NOERROR)
				{
					player->sendCancelMessage(ret);
					return false;
				}

				//changing the position since its now in the inventory of the player
				internalGetPosition(moveItem, itemPos, itemStackPos);
			}

			std::list<Direction> listDir;
			if(getPathToEx(player, walkToPos, listDir, 0, 1, true, true))
			{
				g_dispatcher.addTask(createTask(boost::bind(&Game::playerAutoWalk,
					this, player->getID(), listDir)));

				SchedulerTask* task = createSchedulerTask(400, boost::bind(&Game::playerUseItemEx, this,
					playerId, itemPos, itemStackPos, fromSpriteId, toPos, toStackPos, toSpriteId, isHotkey));
				player->setNextWalkActionTask(task);
				return true;
			}
			else
			{
				player->sendCancelMessage(RET_THEREISNOWAY);
				return false;
			}
		}

		player->sendCancelMessage(ret);
		return false;
	}

	if(!player->canDoAction())
	{
		uint32_t delay = player->getNextActionTime();
		SchedulerTask* task = createSchedulerTask(delay, boost::bind(&Game::playerUseItemEx, this,
			playerId, fromPos, fromStackPos, fromSpriteId, toPos, toStackPos, toSpriteId, isHotkey));
		player->setNextActionTask(task);
		return false;
	}

	player->resetIdleTime();
	player->setNextActionTask(NULL);

	return g_actions->useItemEx(player, fromPos, toPos, toStackPos, item, isHotkey);
}

bool Game::playerUseItem(uint32_t playerId, const Position& pos, uint8_t stackPos,
	uint8_t index, uint16_t spriteId, bool isHotkey)
{
	Player* player = getPlayerByID(playerId);
	if(!player || player->isRemoved())
		return false;

	if(isHotkey && !g_config.getBoolean(ConfigManager::AIMBOT_HOTKEY_ENABLED))
		return false;

	Thing* thing = internalGetThing(player, pos, stackPos, spriteId, STACKPOS_USEITEM);
	if(!thing)
	{
		player->sendCancelMessage(RET_NOTPOSSIBLE);
		return false;
	}

	Item* item = thing->getItem();
	if(!item || item->isUseable())
	{
		player->sendCancelMessage(RET_CANNOTUSETHISOBJECT);
		return false;
	}

	ReturnValue ret = g_actions->canUse(player, pos);
	if(ret != RET_NOERROR)
	{
		if(ret == RET_TOOFARAWAY)
		{
			std::list<Direction> listDir;
			if(getPathToEx(player, pos, listDir, 0, 1, true, true))
			{
				g_dispatcher.addTask(createTask(boost::bind(&Game::playerAutoWalk,
					this, player->getID(), listDir)));

				SchedulerTask* task = createSchedulerTask(400, boost::bind(&Game::playerUseItem, this,
					playerId, pos, stackPos, index, spriteId, isHotkey));
				player->setNextWalkActionTask(task);
				return true;
			}
			ret = RET_THEREISNOWAY;
		}
		player->sendCancelMessage(ret);
		return false;
	}

	if(!player->canDoAction())
	{
		uint32_t delay = player->getNextActionTime();
		SchedulerTask* task = createSchedulerTask(delay, boost::bind(&Game::playerUseItem, this,
			playerId, pos, stackPos, index, spriteId, isHotkey));
		player->setNextActionTask(task);
		return false;
	}

	player->resetIdleTime();
	player->setNextActionTask(NULL);

	g_actions->useItem(player, pos, index, item, isHotkey);
	return true;
}

bool Game::playerUseBattleWindow(uint32_t playerId, const Position& fromPos, uint8_t fromStackPos,
	uint32_t creatureId, uint16_t spriteId, bool isHotkey)
{
	Player* player = getPlayerByID(playerId);
	if(!player || player->isRemoved())
		return false;

	Creature* creature = getCreatureByID(creatureId);
	if(!creature)
		return false;

	if(!Position::areInRange<7,5,0>(creature->getPosition(), player->getPosition()))
		return false;

	if(!g_config.getBoolean(ConfigManager::AIMBOT_HOTKEY_ENABLED))
	{
		if(creature->getPlayer() || isHotkey)
		{
			player->sendCancelMessage(RET_DIRECTPLAYERSHOOT);
			return false;
		}
	}

	Thing* thing = internalGetThing(player, fromPos, fromStackPos, spriteId, STACKPOS_USE);
	if(!thing)
	{
		player->sendCancelMessage(RET_NOTPOSSIBLE);
		return false;
	}

	Item* item = thing->getItem();
	if(!item || item->getClientID() != spriteId)
	{
		player->sendCancelMessage(RET_CANNOTUSETHISOBJECT);
		return false;
	}

	ReturnValue ret = g_actions->canUse(player, fromPos);
	if(ret != RET_NOERROR)
	{
		if(ret == RET_TOOFARAWAY)
		{
			std::list<Direction> listDir;
			if(getPathToEx(player, item->getPosition(), listDir, 0, 1, true, true))
			{
				g_dispatcher.addTask(createTask(boost::bind(&Game::playerAutoWalk,
					this, player->getID(), listDir)));

				SchedulerTask* task = createSchedulerTask(400, boost::bind(&Game::playerUseBattleWindow, this,
					playerId, fromPos, fromStackPos, creatureId, spriteId, isHotkey));
				player->setNextWalkActionTask(task);
				return true;
			}
			ret = RET_THEREISNOWAY;
		}
		player->sendCancelMessage(ret);
		return false;
	}

	if(!player->canDoAction())
	{
		uint32_t delay = player->getNextActionTime();
		SchedulerTask* task = createSchedulerTask(delay, boost::bind(&Game::playerUseBattleWindow, this,
			playerId, fromPos, fromStackPos, creatureId, spriteId, isHotkey));
		player->setNextActionTask(task);
		return false;
	}

	player->setNextActionTask(NULL);
	player->resetIdleTime();

	return g_actions->useItemEx(player, fromPos, creature->getPosition(), creature->getParent()->__getIndexOfThing(creature), item, isHotkey, creatureId);
}

bool Game::playerCloseContainer(uint32_t playerId, uint8_t cid)
{
	Player* player = getPlayerByID(playerId);
	if(!player || player->isRemoved())
		return false;

	player->closeContainer(cid);
	player->sendCloseContainer(cid);
	return true;
}

bool Game::playerMoveUpContainer(uint32_t playerId, uint8_t cid)
{
	Player* player = getPlayerByID(playerId);
	if(!player || player->isRemoved())
		return false;

	Container* container = player->getContainer(cid);
	if(!container)
		return false;

	Container* parentContainer = dynamic_cast<Container*>(container->getParent());
	if(!parentContainer)
		return false;

	bool hasParent = (dynamic_cast<const Container*>(parentContainer->getParent()) != NULL);
	player->addContainer(cid, parentContainer);
	player->sendContainer(cid, parentContainer, hasParent);
	return true;
}

bool Game::playerUpdateTile(uint32_t playerId, const Position& pos)
{
	Player* player = getPlayerByID(playerId);
	if(!player || player->isRemoved())
		return false;

	if(player->canSee(pos))
	{
		Tile* tile = getTile(pos.x, pos.y, pos.z);
		player->sendUpdateTile(tile, pos);
		return true;
	}
	return false;
}

bool Game::playerUpdateContainer(uint32_t playerId, uint8_t cid)
{
	Player* player = getPlayerByID(playerId);
	if(!player || player->isRemoved())
		return false;

	Container* container = player->getContainer(cid);
	if(!container)
		return false;

	bool hasParent = (dynamic_cast<const Container*>(container->getParent()) != NULL);
	player->sendContainer(cid, container, hasParent);
	return true;
}

bool Game::playerRotateItem(uint32_t playerId, const Position& pos, uint8_t stackPos, const uint16_t spriteId)
{
	Player* player = getPlayerByID(playerId);
	if(!player || player->isRemoved())
		return false;

	Thing* thing = internalGetThing(player, pos, stackPos);
	if(!thing)
		return false;

	Item* item = thing->getItem();
	if(!item || item->getClientID() != spriteId || !item->isRoteable() || item->getUniqueId() != 0)
	{
		player->sendCancelMessage(RET_NOTPOSSIBLE);
		return false;
	}

	if(pos.x != 0xFFFF && !Position::areInRange<1,1,0>(pos, player->getPosition()))
	{
		std::list<Direction> listDir;
		if(getPathToEx(player, pos, listDir, 0, 1, true, true))
		{
			g_dispatcher.addTask(createTask(boost::bind(&Game::playerAutoWalk,
				this, player->getID(), listDir)));

			SchedulerTask* task = createSchedulerTask(400, boost::bind(&Game::playerRotateItem, this,
				playerId, pos, stackPos, spriteId));
			player->setNextWalkActionTask(task);
			return true;
		}
		else
		{
			player->sendCancelMessage(RET_THEREISNOWAY);
			return false;
		}
	}

	uint16_t newId = Item::items[item->getID()].rotateTo;
	if(newId != 0)
		transformItem(item, newId);

	return true;
}

bool Game::playerWriteItem(uint32_t playerId, uint32_t windowTextId, const std::string& text)
{
	Player* player = getPlayerByID(playerId);
	if(!player || player->isRemoved())
		return false;

	uint16_t maxTextLength = 0;
	uint32_t internalWindowTextId = 0;
	Item* writeItem = player->getWriteItem(internalWindowTextId, maxTextLength);

	if(text.length() > maxTextLength || windowTextId != internalWindowTextId)
		return false;

	if(!writeItem || writeItem->isRemoved())
	{
		player->sendCancelMessage(RET_NOTPOSSIBLE);
		return false;
	}

	Cylinder* topParent = writeItem->getTopParent();
	Player* owner = dynamic_cast<Player*>(topParent);
	if(owner && owner != player)
	{
		player->sendCancelMessage(RET_NOTPOSSIBLE);
		return false;
	}

	if(!Position::areInRange<1,1,0>(writeItem->getPosition(), player->getPosition()))
	{
		player->sendCancelMessage(RET_NOTPOSSIBLE);
		return false;
	}

	if(!text.empty())
	{
		if(writeItem->getText() != text)
		{
			writeItem->setText(text);
			writeItem->setWriter(player->getName());
			writeItem->setDate(std::time(NULL));
		}
	}
	else
	{
		writeItem->resetText();
		writeItem->resetWriter();
		writeItem->resetDate();
	}

	uint16_t newId = Item::items[writeItem->getID()].writeOnceItemId;
	if(newId != 0)
		transformItem(writeItem, newId);

	player->setWriteItem(NULL);
	return true;
}

bool Game::playerUpdateHouseWindow(uint32_t playerId, uint8_t listId, uint32_t windowTextId, const std::string& text)
{
	Player* player = getPlayerByID(playerId);
	if(!player || player->isRemoved())
		return false;

	uint32_t internalWindowTextId;
	uint32_t internalListId;
	House* house = player->getEditHouse(internalWindowTextId, internalListId);
	if(house && internalWindowTextId == windowTextId && listId == 0)
	{
		house->setAccessList(internalListId, text);
		player->setEditHouse(NULL);
	}
	return true;
}

bool Game::playerRequestTrade(uint32_t playerId, const Position& pos, uint8_t stackPos,
	uint32_t tradePlayerId, uint16_t spriteId)
{
	Player* player = getPlayerByID(playerId);
	if(!player || player->isRemoved())
		return false;

	Player* tradePartner = getPlayerByID(tradePlayerId);
	if(!tradePartner || tradePartner == player)
	{
		player->sendTextMessage(MSG_INFO_DESCR, "Sorry, not possible.");
		return false;
	}

	if(!Position::areInRange<2,2,0>(tradePartner->getPosition(), player->getPosition()))
	{
		std::ostringstream ss;
		ss << tradePartner->getName() << " tells you to move closer.";
		player->sendTextMessage(MSG_INFO_DESCR, ss.str());
		return false;
	}

	if(!canThrowObjectTo(tradePartner->getPosition(), player->getPosition()))
	{
		player->sendCancelMessage(RET_CREATUREISNOTREACHABLE);
		return false;
	}

	Item* tradeItem = dynamic_cast<Item*>(internalGetThing(player, pos, stackPos, spriteId, STACKPOS_USE));
	if(!tradeItem || tradeItem->getClientID() != spriteId || !tradeItem->isPickupable() || tradeItem->getUniqueId() != 0)
	{
		player->sendCancelMessage(RET_NOTPOSSIBLE);
		return false;
	}

	const Position& playerPosition = player->getPosition();
	const Position& tradeItemPosition = tradeItem->getPosition();
	if(playerPosition.z > tradeItemPosition.z)
	{
		player->sendCancelMessage(RET_FIRSTGOUPSTAIRS);
		return false;
	}
	else if(playerPosition.z < tradeItemPosition.z)
	{
		player->sendCancelMessage(RET_FIRSTGODOWNSTAIRS);
		return false;
	}
	else if(!Position::areInRange<1,1,0>(tradeItemPosition, playerPosition))
	{
		std::list<Direction> listDir;
		if(getPathToEx(player, pos, listDir, 0, 1, true, true))
		{
			g_dispatcher.addTask(createTask(boost::bind(&Game::playerAutoWalk,
				this, player->getID(), listDir)));

			SchedulerTask* task = createSchedulerTask(400, boost::bind(&Game::playerRequestTrade, this,
				playerId, pos, stackPos, tradePlayerId, spriteId));
			player->setNextWalkActionTask(task);
			return true;
		}
		else
		{
			player->sendCancelMessage(RET_THEREISNOWAY);
			return false;
		}
	}

	const Container* container = NULL;
	std::map<Item*, uint32_t>::const_iterator it;
	for(it = tradeItems.begin(); it != tradeItems.end(); ++it)
	{
		if(tradeItem == it->first ||
			((container = dynamic_cast<const Container*>(tradeItem)) && container->isHoldingItem(it->first)) ||
			((container = dynamic_cast<const Container*>(it->first)) && container->isHoldingItem(tradeItem)))
		{
			player->sendTextMessage(MSG_INFO_DESCR, "This item is already being traded.");
			return false;
		}
	}

	Container* tradeContainer = tradeItem->getContainer();
	if(tradeContainer && tradeContainer->getItemHoldingCount() + 1 > 100)
	{
		player->sendTextMessage(MSG_INFO_DESCR, "You can not trade more than 100 items.");
		return false;
	}
	return internalStartTrade(player, tradePartner, tradeItem);
}

bool Game::internalStartTrade(Player* player, Player* tradePartner, Item* tradeItem)
{
	if(player->tradeState != TRADE_NONE && !(player->tradeState == TRADE_ACKNOWLEDGE && player->tradePartner == tradePartner))
	{
		player->sendCancelMessage(RET_YOUAREALREADYTRADING);
		return false;
	}
	else if(tradePartner->tradeState != TRADE_NONE && tradePartner->tradePartner != player)
	{
		player->sendCancelMessage(RET_THISPLAYERISALREADYTRADING);
		return false;
	}

	player->tradePartner = tradePartner;
	player->tradeItem = tradeItem;
	player->tradeState = TRADE_INITIATED;
	tradeItem->useThing2();
	tradeItems[tradeItem] = player->getID();

	player->sendTradeItemRequest(player, tradeItem, true);

	if(tradePartner->tradeState == TRADE_NONE)
	{
		std::ostringstream ss;
		ss << player->getName() << " wants to trade with you";
		tradePartner->sendTextMessage(MSG_INFO_DESCR, ss.str());
		tradePartner->tradeState = TRADE_ACKNOWLEDGE;
		tradePartner->tradePartner = player;
	}
	else
	{
		Item* counterOfferItem = tradePartner->tradeItem;
		player->sendTradeItemRequest(tradePartner, counterOfferItem, false);
		tradePartner->sendTradeItemRequest(player, tradeItem, false);
	}
	return true;
}

bool Game::playerAcceptTrade(uint32_t playerId)
{
	Player* player = getPlayerByID(playerId);
	if(!player || player->isRemoved())
		return false;

	if(!(player->getTradeState() == TRADE_ACKNOWLEDGE || player->getTradeState() == TRADE_INITIATED))
		return false;

	Player* tradePartner = player->tradePartner;
	if(!tradePartner)
		return false;

	if(!canThrowObjectTo(tradePartner->getPosition(), player->getPosition()))
	{
		player->sendCancelMessage(RET_CREATUREISNOTREACHABLE);
		return false;
	}

	player->setTradeState(TRADE_ACCEPT);
	if(tradePartner->getTradeState() == TRADE_ACCEPT)
	{
		Item* tradeItem1 = player->tradeItem;
		Item* tradeItem2 = tradePartner->tradeItem;

		player->setTradeState(TRADE_TRANSFER);
		tradePartner->setTradeState(TRADE_TRANSFER);

		std::map<Item*, uint32_t>::iterator it;

		it = tradeItems.find(tradeItem1);
		if(it != tradeItems.end())
		{
			FreeThing(it->first);
			tradeItems.erase(it);
		}

		it = tradeItems.find(tradeItem2);
		if(it != tradeItems.end())
		{
			FreeThing(it->first);
			tradeItems.erase(it);
		}

		bool isSuccess = false;

		ReturnValue ret1 = internalAddItem(tradePartner, tradeItem1, INDEX_WHEREEVER, 0, true);
		ReturnValue ret2 = internalAddItem(player, tradeItem2, INDEX_WHEREEVER, 0, true);

		if(ret1 == RET_NOERROR && ret2 == RET_NOERROR)
		{
			ret1 = internalRemoveItem(tradeItem1, tradeItem1->getItemCount(), true);
			ret2 = internalRemoveItem(tradeItem2, tradeItem2->getItemCount(), true);
			if(ret1 == RET_NOERROR && ret2 == RET_NOERROR)
			{
				Cylinder* cylinder1 = tradeItem1->getParent();
				Cylinder* cylinder2 = tradeItem2->getParent();

				uint32_t count1 = tradeItem1->getItemCount();
				uint32_t count2 = tradeItem2->getItemCount();

				internalMoveItem(cylinder1, tradePartner, INDEX_WHEREEVER, tradeItem1, count1, NULL, FLAG_IGNOREAUTOSTACK);
				internalMoveItem(cylinder2, player, INDEX_WHEREEVER, tradeItem2, count2, NULL, FLAG_IGNOREAUTOSTACK);

				tradeItem1->onTradeEvent(ON_TRADE_TRANSFER, tradePartner);
				tradeItem2->onTradeEvent(ON_TRADE_TRANSFER, player);

				isSuccess = true;
			}
		}

		if(!isSuccess)
		{
			std::string errorDescription;
			if(tradePartner->tradeItem)
			{
				errorDescription = getTradeErrorDescription(ret1, tradeItem1);
				tradePartner->sendTextMessage(MSG_INFO_DESCR, errorDescription);
				tradePartner->tradeItem->onTradeEvent(ON_TRADE_CANCEL, tradePartner);
			}

			if(player->tradeItem)
			{
				errorDescription = getTradeErrorDescription(ret2, tradeItem2);
				player->sendTextMessage(MSG_INFO_DESCR, errorDescription);
				player->tradeItem->onTradeEvent(ON_TRADE_CANCEL, player);
			}
		}

		player->setTradeState(TRADE_NONE);
		player->tradeItem = NULL;
		player->tradePartner = NULL;
		player->sendTradeClose();

		tradePartner->setTradeState(TRADE_NONE);
		tradePartner->tradeItem = NULL;
		tradePartner->tradePartner = NULL;
		tradePartner->sendTradeClose();
		return isSuccess;
	}
	return false;
}

std::string Game::getTradeErrorDescription(ReturnValue ret, Item* item)
{
	std::ostringstream ss;
	if(item)
	{
		if(ret == RET_NOTENOUGHCAPACITY)
		{
			ss << "You do not have enough capacity to carry";
			if(item->isStackable() && item->getItemCount() > 1)
				ss << " these objects.";
			else
				ss << " this object." ;

			ss << std::endl << " " << item->getWeightDescription();
		}
		else if(ret == RET_NOTENOUGHROOM || ret == RET_CONTAINERNOTENOUGHROOM)
		{
			ss << "You do not have enough room to carry";
			if(item->isStackable() && item->getItemCount() > 1)
				ss << " these objects.";
			else
				ss << " this object.";
		}
		return ss.str();
	}

	ss << "Trade could not be completed.";
	return ss.str().c_str();
}

bool Game::playerLookInTrade(uint32_t playerId, bool lookAtCounterOffer, int index)
{
	Player* player = getPlayerByID(playerId);
	if(!player || player->isRemoved())
		return false;

	Player* tradePartner = player->tradePartner;
	if(!tradePartner)
		return false;

	Item* tradeItem = NULL;

	if(lookAtCounterOffer)
		tradeItem = tradePartner->getTradeItem();
	else
		tradeItem = player->getTradeItem();

	if(!tradeItem)
		return false;

	const Position& playerPosition = player->getPosition();
	const Position& tradeItemPosition = tradeItem->getPosition();
	int32_t lookDistance = std::max(std::abs(playerPosition.x - tradeItemPosition.x),
		std::abs(playerPosition.y - tradeItemPosition.y));

	std::ostringstream ss;
	if(index == 0)
	{
		ss << "You see " << tradeItem->getDescription(lookDistance);
		player->sendTextMessage(MSG_INFO_DESCR, ss.str());
		return false;
	}

	Container* tradeContainer = tradeItem->getContainer();
	if(!tradeContainer || index > (int32_t)tradeContainer->getItemHoldingCount())
		return false;

	bool foundItem = false;
	std::list<const Container*> listContainer;
	ItemList::const_iterator it;
	Container* tmpContainer = NULL;

	listContainer.push_back(tradeContainer);
	while(!foundItem && !listContainer.empty())
	{
		const Container* container = listContainer.front();
		listContainer.pop_front();
		for(it = container->getItems(); it != container->getEnd(); ++it)
		{
			if((tmpContainer = (*it)->getContainer()))
				listContainer.push_back(tmpContainer);
			--index;
			if(index == 0)
			{
				tradeItem = *it;
				foundItem = true;
				break;
			}
		}
	}

	if(foundItem)
	{
		ss << "You see " << tradeItem->getDescription(lookDistance);
		player->sendTextMessage(MSG_INFO_DESCR, ss.str());
	}
	return foundItem;
}

bool Game::playerCloseTrade(uint32_t playerId)
{
	Player* player = getPlayerByID(playerId);
	if(!player || player->isRemoved())
		return false;

	return internalCloseTrade(player);
}

bool Game::internalCloseTrade(Player* player)
{
	Player* tradePartner = player->tradePartner;
	if((tradePartner && tradePartner->getTradeState() == TRADE_TRANSFER) || player->getTradeState() == TRADE_TRANSFER)
	{
		std::cout << "Warning: [Game::playerCloseTrade] TradeState == TRADE_TRANSFER. " <<
			player->getName() << " " << player->getTradeState() << " , " <<
			tradePartner->getName() << " " << tradePartner->getTradeState() << std::endl;
		return true;
	}

	std::vector<Item*>::iterator it;
	if(player->getTradeItem())
	{
		std::map<Item*, uint32_t>::iterator it = tradeItems.find(player->getTradeItem());
		if(it != tradeItems.end())
		{
			FreeThing(it->first);
			tradeItems.erase(it);
		}
		player->tradeItem->onTradeEvent(ON_TRADE_CANCEL, player);
		player->tradeItem = NULL;
	}

	player->setTradeState(TRADE_NONE);
	player->tradePartner = NULL;

	player->sendTextMessage(MSG_STATUS_SMALL, "Trade cancelled.");
	player->sendTradeClose();

	if(tradePartner)
	{
		if(tradePartner->getTradeItem())
		{
			std::map<Item*, uint32_t>::iterator it = tradeItems.find(tradePartner->getTradeItem());
			if(it != tradeItems.end())
			{
				FreeThing(it->first);
				tradeItems.erase(it);
			}

			tradePartner->tradeItem->onTradeEvent(ON_TRADE_CANCEL, tradePartner);
			tradePartner->tradeItem = NULL;
		}

		tradePartner->setTradeState(TRADE_NONE);
		tradePartner->tradePartner = NULL;

		tradePartner->sendTextMessage(MSG_STATUS_SMALL, "Trade cancelled.");
		tradePartner->sendTradeClose();
	}
	return true;
}

bool Game::playerPurchaseItem(uint32_t playerId, uint16_t spriteId, uint8_t count, uint8_t amount,
	bool ignoreCap/* = false*/, bool inBackpacks/* = false*/)
{
	if(amount == 0 || amount > 100)
		return false;

	Player* player = getPlayerByID(playerId);
	if(player == NULL || player->isRemoved())
		return false;

	int32_t onBuy;
	int32_t onSell;

	Npc* merchant = player->getShopOwner(onBuy, onSell);
	if(merchant == NULL)
		return false;

	const ItemType& it = Item::items.getItemIdByClientId(spriteId);
	if(it.id == 0)
		return false;

	uint8_t subType;
	if(it.isSplash() || it.isFluidContainer())
		subType = clientFluidToServer(count);
	else
		subType = count;

	if(!player->hasShopItemForSale(it.id, subType))
		return false;

	merchant->onPlayerTrade(player, SHOPEVENT_BUY, onBuy, it.id, subType, amount, ignoreCap, inBackpacks);
	return true;
}

bool Game::playerSellItem(uint32_t playerId, uint16_t spriteId, uint8_t count,
	uint8_t amount)
{
	if(amount == 0 || amount > 100)
		return false;

	Player* player = getPlayerByID(playerId);
	if(player == NULL || player->isRemoved())
		return false;

	int32_t onBuy;
	int32_t onSell;

	Npc* merchant = player->getShopOwner(onBuy, onSell);
	if(merchant == NULL)
		return false;

	const ItemType& it = Item::items.getItemIdByClientId(spriteId);
	if(it.id == 0)
		return false;

	uint8_t subType;
	if(it.isSplash() || it.isFluidContainer())
		subType = clientFluidToServer(count);
	else
		subType = count;

	merchant->onPlayerTrade(player, SHOPEVENT_SELL, onSell, it.id, subType, amount);
	return true;
}

bool Game::playerCloseShop(uint32_t playerId)
{
	Player* player = getPlayerByID(playerId);
	if(player == NULL || player->isRemoved())
		return false;

	player->closeShopWindow();
	return true;
}

bool Game::playerLookInShop(uint32_t playerId, uint16_t spriteId, uint8_t count)
{
	Player* player = getPlayerByID(playerId);
	if(player == NULL || player->isRemoved())
		return false;

	const ItemType& it = Item::items.getItemIdByClientId(spriteId);
	if(it.id == 0)
		return false;

	int32_t subType;
	if(it.isFluidContainer() || it.isSplash())
		subType = clientFluidToServer(count);
	else
		subType = count;

	std::ostringstream ss;
	ss << "You see " << Item::getDescription(it, 1, NULL, subType);
	player->sendTextMessage(MSG_INFO_DESCR, ss.str());
	return true;
}

bool Game::playerLookAt(uint32_t playerId, const Position& pos, uint16_t spriteId, uint8_t stackPos)
{
	Player* player = getPlayerByID(playerId);
	if(!player || player->isRemoved())
		return false;

	Thing* thing = internalGetThing(player, pos, stackPos, spriteId, STACKPOS_LOOK);
	if(!thing)
	{
		player->sendCancelMessage(RET_NOTPOSSIBLE);
		return false;
	}

	Position thingPos = thing->getPosition();
	if(!player->canSee(thingPos))
	{
		player->sendCancelMessage(RET_NOTPOSSIBLE);
		return false;
	}

	Position playerPos = player->getPosition();

	int32_t lookDistance = -1;
	if(thing != player)
	{
		lookDistance = std::max(std::abs(playerPos.x - thingPos.x), std::abs(playerPos.y - thingPos.y));
		if(playerPos.z != thingPos.z)
			lookDistance += 15;
	}

	std::ostringstream ss;
	ss << "You see " << thing->getDescription(lookDistance);
	if(player->isAccessPlayer())
	{
		Item* item = thing->getItem();
		if(item)
		{
			ss << std::endl << "ItemID: [" << item->getID() << "]";
			if(item->getActionId() > 0)
				ss << ", ActionID: [" << item->getActionId() << "]";

			if(item->getUniqueId() > 0)
				ss << ", UniqueID: [" << item->getUniqueId() << "]";

			ss << ".";
			const ItemType& it = Item::items[item->getID()];
			if(it.transformEquipTo)
				ss << std::endl << "TransformTo: [" << it.transformEquipTo << "] (onEquip).";
			else if(it.transformDeEquipTo)
				ss << std::endl << "TransformTo: [" << it.transformDeEquipTo << "] (onDeEquip).";

			if(it.decayTo != -1)
				ss << std::endl << "DecayTo: [" << it.decayTo << "].";
		}

		if(const Creature* creature = thing->getCreature())
		{
			ss << std::endl << "Health: [" << creature->getHealth() << " / " << creature->getMaxHealth() << "]";
			if(creature->getMaxMana() > 0)
				ss << ", Mana: [" << creature->getMana() << " / " << creature->getMaxMana() << "]";

			ss << ".";
		}

		ss << std::endl << "Position: [X: " << thingPos.x << "] [Y: " << thingPos.y << "] [Z: " << thingPos.z << "].";
	}

	player->sendTextMessage(MSG_INFO_DESCR, ss.str());
	return true;
}

bool Game::playerLookInBattleList(uint32_t playerId, uint32_t creatureId)
{
	Player* player = getPlayerByID(playerId);
	if(!player || player->isRemoved())
		return false;

	Creature* creature = getCreatureByID(creatureId);
	if(!creature || creature->isRemoved())
		return false;

	if(!player->canSeeCreature(creature))
		return false;

	const Position& creaturePos = creature->getPosition();
	if(!player->canSee(creaturePos))
		return false;

	int32_t lookDistance;
	if(creature != player)
	{
		const Position& playerPos = player->getPosition();
		lookDistance = std::max(std::abs(playerPos.x - creaturePos.x), std::abs(playerPos.y - creaturePos.y));
		if(playerPos.z != creaturePos.z)
			lookDistance += 15;
	}
	else
		lookDistance = -1;

	std::ostringstream ss;
	ss << "You see " << creature->getDescription(lookDistance);
	if(player->isAccessPlayer())
	{
		ss << std::endl << "Health: [" << creature->getHealth() << " / " << creature->getMaxHealth() << "]";
		if(creature->getMaxMana() > 0)
			ss << ", Mana: [" << creature->getMana() << " / " << creature->getMaxMana() << "]";

		ss << "." << std::endl;
		ss << "Position: [X: " << creaturePos.x << "] [Y: " << creaturePos.y << "] [Z: " << creaturePos.z << "].";
	}

	player->sendTextMessage(MSG_INFO_DESCR, ss.str());
	return true;
}

bool Game::playerCancelAttackAndFollow(uint32_t playerId)
{
	Player* player = getPlayerByID(playerId);
	if(!player || player->isRemoved())
		return false;

	playerSetAttackedCreature(playerId, 0);
	playerFollowCreature(playerId, 0);
	player->stopWalk();
	return true;
}

bool Game::playerSetAttackedCreature(uint32_t playerId, uint32_t creatureId)
{
	Player* player = getPlayerByID(playerId);
	if(!player || player->isRemoved())
		return false;

	if(player->getAttackedCreature() && creatureId == 0)
	{
		player->setAttackedCreature(NULL);
		player->sendCancelTarget();
		return true;
	}

	Creature* attackCreature = getCreatureByID(creatureId);
	if(!attackCreature)
	{
		player->setAttackedCreature(NULL);
		player->sendCancelTarget();
		return false;
	}

	ReturnValue ret = Combat::canTargetCreature(player, attackCreature);
	if(ret != RET_NOERROR)
	{
		player->sendCancelMessage(ret);
		player->sendCancelTarget();
		player->setAttackedCreature(NULL);
		return false;
	}

	player->setAttackedCreature(attackCreature);
	g_dispatcher.addTask(createTask(boost::bind(&Game::updateCreatureWalk, this, player->getID())));
	return true;
}

bool Game::playerFollowCreature(uint32_t playerId, uint32_t creatureId)
{
	Player* player = getPlayerByID(playerId);
	if(!player || player->isRemoved())
		return false;

	player->setAttackedCreature(NULL);
	Creature* followCreature = NULL;
	if(creatureId != 0)
		followCreature = getCreatureByID(creatureId);

	g_dispatcher.addTask(createTask(boost::bind(&Game::updateCreatureWalk, this, player->getID())));
	return player->setFollowCreature(followCreature);
}

bool Game::playerSetFightModes(uint32_t playerId, fightMode_t fightMode, chaseMode_t chaseMode, secureMode_t secureMode)
{
	Player* player = getPlayerByID(playerId);
	if(!player || player->isRemoved())
		return false;

	player->setFightMode(fightMode);
	player->setChaseMode(chaseMode);
	player->setSecureMode(secureMode);
	return true;
}

bool Game::playerRequestAddVip(uint32_t playerId, const std::string& vip_name)
{
	if(vip_name.size() > 32)
		return false;

	Player* player = getPlayerByID(playerId);
	if(!player || player->isRemoved())
		return false;

	std::string real_name;
	real_name = vip_name;
	uint32_t guid;
	bool specialVip;

	if(!IOLoginData::getInstance()->getGuidByNameEx(guid, specialVip, real_name))
	{
		player->sendTextMessage(MSG_STATUS_SMALL, "A player with that name does not exist.");
		return false;
	}

	if(specialVip && !player->hasFlag(PlayerFlag_SpecialVIP))
	{
		player->sendTextMessage(MSG_STATUS_SMALL, "You can not add this player.");
		return false;
	}

	bool online = (getPlayerByName(real_name) != NULL);
	return player->addVIP(guid, real_name, online);
}

bool Game::playerRequestRemoveVip(uint32_t playerId, uint32_t guid)
{
	Player* player = getPlayerByID(playerId);
	if(!player || player->isRemoved())
		return false;

	player->removeVIP(guid);
	return true;
}

bool Game::playerTurn(uint32_t playerId, Direction dir)
{
	Player* player = getPlayerByID(playerId);
	if(!player || player->isRemoved())
		return false;

	player->resetIdleTime();
	return internalCreatureTurn(player, dir);
}

bool Game::playerRequestOutfit(uint32_t playerId)
{
	if(!g_config.getBoolean(ConfigManager::ALLOW_CHANGEOUTFIT))
		return false;

	Player* player = getPlayerByID(playerId);
	if(!player || player->isRemoved())
		return false;

	player->sendOutfitWindow();
	return true;
}

bool Game::playerToggleMount(uint32_t playerId, bool mount)
{
	Player* player = getPlayerByID(playerId);
	if(!player || player->isRemoved())
		return false;

	return player->toggleMount(mount);
}

bool Game::playerChangeOutfit(uint32_t playerId, Outfit_t outfit)
{
	if(!g_config.getBoolean(ConfigManager::ALLOW_CHANGEOUTFIT))
		return false;

	Player* player = getPlayerByID(playerId);
	if(!player || player->isRemoved())
		return false;

	if(outfit.lookMount != 0)
	{
		Mount* mount = Mounts::getInstance()->getMountByClientID(outfit.lookMount);
		if(!mount || !mount->isTamed(player))
			return false;

		if(player->isMounted())
		{
			Mount* prevMount = Mounts::getInstance()->getMountByID(player->getCurrentMount());
			if(prevMount)
				changeSpeed(player, mount->getSpeed() - prevMount->getSpeed());

			player->setCurrentMount(mount->getID());
		}
		else
		{
			player->setCurrentMount(mount->getID());
			outfit.lookMount = 0;
		}
	}
	else if(player->isMounted())
		player->dismount();

	if(player->canWear(outfit.lookType, outfit.lookAddons) && player->hasRequestedOutfit())
	{
		player->hasRequestedOutfit(false);
		player->defaultOutfit = outfit;
		if(player->hasCondition(CONDITION_OUTFIT))
			return false;

		internalCreatureChangeOutfit(player, outfit);
	}
	return true;
}

bool Game::playerShowQuestLog(uint32_t playerId)
{
	Player* player = getPlayerByID(playerId);
	if(!player || player->isRemoved())
		return false;

	player->sendQuestLog();
	return true;
}

bool Game::playerShowQuestLine(uint32_t playerId, uint16_t questId)
{
	Player* player = getPlayerByID(playerId);
	if(!player || player->isRemoved())
		return false;

	Quest* quest = Quests::getInstance()->getQuestByID(questId);
	if(!quest)
		return false;

	player->sendQuestLine(quest);
	return true;
}

bool Game::playerSay(uint32_t playerId, uint16_t channelId, SpeakClasses type,
	const std::string& receiver, const std::string& text)
{
	Player* player = getPlayerByID(playerId);
	if(!player || player->isRemoved())
		return false;

	player->resetIdleTime();

	uint32_t muteTime = player->isMuted();
	if(muteTime > 0)
	{
		std::ostringstream ss;
		ss << "You are still muted for " << muteTime << " seconds.";
		player->sendTextMessage(MSG_STATUS_SMALL, ss.str());
		return false;
	}

	if(playerSayCommand(player, type, text))
		return true;

	if(playerSaySpell(player, type, text))
		return true;

	player->removeMessageBuffer();

	switch(type)
	{
		case SPEAK_SAY:
			return internalCreatureSay(player, SPEAK_SAY, text, false);

		case SPEAK_WHISPER:
			return playerWhisper(player, text);

		case SPEAK_YELL:
			return playerYell(player, text);

		case SPEAK_PRIVATE_TO:
		case SPEAK_PRIVATE_RED_TO:
			return playerSpeakTo(player, type, receiver, text);

		case SPEAK_CHANNEL_O:
		case SPEAK_CHANNEL_Y:
		case SPEAK_CHANNEL_R1:
			return playerTalkToChannel(player, type, text, channelId);

		case SPEAK_PRIVATE_PN:
			return playerSpeakToNpc(player, text);

		case SPEAK_BROADCAST:
			return playerBroadcastMessage(player, text);

		default:
			break;
	}
	return false;
}

bool Game::playerSayCommand(Player* player, SpeakClasses type, const std::string& text)
{
	if(player->isAccountManager())
		return internalCreatureSay(player, SPEAK_SAY, text, false);

	//First, check if this was a command
	for(uint32_t i = 0; i < commandTags.size(); i++)
	{
		if(commandTags[i] == text.substr(0,1))
		{
			if(commands.exeCommand(player, text))
				return true;
		}
	}
	return false;
}

bool Game::playerSaySpell(Player* player, SpeakClasses type, const std::string& text)
{
	if(player->isAccountManager())
		return internalCreatureSay(player, SPEAK_SAY, text, false);

	std::string words = text;
	TalkActionResult_t result = g_talkActions->playerSaySpell(player, type, words);
	if(result == TALKACTION_BREAK)
		return true;

	result = g_spells->playerSaySpell(player, type, words);
	if(result == TALKACTION_BREAK)
		return internalCreatureSay(player, SPEAK_SAY, words, false);
	else if(result == TALKACTION_FAILED)
		return true;

	return false;
}

bool Game::playerWhisper(Player* player, const std::string& text)
{
	SpectatorVec list;
	getSpectators(list, player->getPosition(), false, false,
		Map::maxClientViewportX, Map::maxClientViewportX,
		Map::maxClientViewportY, Map::maxClientViewportY);
	SpectatorVec::const_iterator it;

	//send to client
	Player* tmpPlayer = NULL;
	for(it = list.begin(); it != list.end(); ++it)
	{
		if((tmpPlayer = (*it)->getPlayer()))
		{
			if(!Position::areInRange<1,1,0>(player->getPosition(), (*it)->getPosition()))
				tmpPlayer->sendCreatureSay(player, SPEAK_WHISPER, "pspsps");
			else
				tmpPlayer->sendCreatureSay(player, SPEAK_WHISPER, text);
		}
	}

	//event method
	for(it = list.begin(); it != list.end(); ++it)
		(*it)->onCreatureSay(player, SPEAK_WHISPER, text);

	return true;
}

bool Game::playerYell(Player* player, const std::string& text)
{
	if(player->getLevel() > 1)
	{
		if(!player->hasCondition(CONDITION_YELLTICKS))
		{
			if(player->getAccountType() < ACCOUNT_TYPE_GAMEMASTER)
			{
				Condition* condition = Condition::createCondition(CONDITIONID_DEFAULT, CONDITION_YELLTICKS, 30000, 0);
				player->addCondition(condition);
			}
			internalCreatureSay(player, SPEAK_YELL, asUpperCaseString(text), false);
		}
		else
			player->sendCancelMessage(RET_YOUAREEXHAUSTED);
	}
	else
		player->sendTextMessage(MSG_STATUS_SMALL, "You may not yell as long as you are on level 1.");

	return true;
}

bool Game::playerSpeakTo(Player* player, SpeakClasses type, const std::string& receiver,
	const std::string& text)
{
	Player* toPlayer = getPlayerByName(receiver);
	if(!toPlayer || toPlayer->isRemoved())
	{
		player->sendTextMessage(MSG_STATUS_SMALL, "A player with this name is not online.");
		return false;
	}

	if(type == SPEAK_PRIVATE_RED_TO && (player->hasFlag(PlayerFlag_CanTalkRedPrivate) || player->getAccountType() >= ACCOUNT_TYPE_GAMEMASTER))
		type = SPEAK_PRIVATE_RED_FROM;
	else
		type = SPEAK_PRIVATE_FROM;

	toPlayer->sendCreatureSay(player, type, text);
	toPlayer->onCreatureSay(player, type, text);

	if(toPlayer->isInGhostMode() && !player->isAccessPlayer())
		player->sendTextMessage(MSG_STATUS_SMALL, "A player with this name is not online.");
	else
	{
		std::ostringstream ss;
		ss << "Message sent to " << toPlayer->getName() << ".";
		player->sendTextMessage(MSG_STATUS_SMALL, ss.str());
	}
	return true;
}

bool Game::playerTalkToChannel(Player* player, SpeakClasses type, const std::string& text, uint16_t channelId)
{
	switch(type)
	{
		case SPEAK_CHANNEL_Y:
		{
			if(player->isAccessPlayer() || (channelId == CHANNEL_HELP && (player->hasFlag(PlayerFlag_TalkOrangeHelpChannel) || player->getAccountType() > ACCOUNT_TYPE_NORMAL)))
				type = SPEAK_CHANNEL_O;

			break;
		}

		case SPEAK_CHANNEL_O:
		{
			if(channelId != CHANNEL_HELP || (!player->hasFlag(PlayerFlag_TalkOrangeHelpChannel) && player->getAccountType() == ACCOUNT_TYPE_NORMAL))
				type = SPEAK_CHANNEL_Y;

			break;
		}

		case SPEAK_CHANNEL_R1:
		{
			if(!player->hasFlag(PlayerFlag_CanTalkRedChannel) && player->getAccountType() < ACCOUNT_TYPE_GAMEMASTER)
			{
				if(channelId == CHANNEL_HELP && (player->hasFlag(PlayerFlag_TalkOrangeHelpChannel) || player->getAccountType() >= ACCOUNT_TYPE_TUTOR))
					type = SPEAK_CHANNEL_O;
				else
					type = SPEAK_CHANNEL_Y;
			}
			break;
		}

		default:
			break;
	}
	return g_chat.talkToChannel(player, type, text, channelId);
}

bool Game::playerSpeakToNpc(Player* player, const std::string& text)
{
	SpectatorVec list;
	SpectatorVec::iterator it;
	getSpectators(list, player->getPosition());

	//send to npcs only
	for(it = list.begin(); it != list.end(); ++it)
	{
		if((*it)->getNpc())
			(*it)->onCreatureSay(player, SPEAK_PRIVATE_PN, text);
	}
	return true;
}

bool Game::npcSpeakToPlayer(Npc* npc, Player* player, const std::string& text, bool publicize)
{
	if(player != NULL)
	{
		player->sendCreatureSay(npc, SPEAK_PRIVATE_NP, text);
		player->onCreatureSay(npc, SPEAK_PRIVATE_NP, text);
	}

	if(publicize)
	{
		SpectatorVec list;
		SpectatorVec::iterator it;
		getSpectators(list, npc->getPosition());

		//send to client
		Player* tmpPlayer = NULL;
		for(it = list.begin(); it != list.end(); ++it)
		{
			tmpPlayer = (*it)->getPlayer();
			if((tmpPlayer != NULL) && (tmpPlayer != player))
				tmpPlayer->sendCreatureSay(npc, SPEAK_SAY, text);
		}

		//event method
		for(it = list.begin(); it != list.end(); ++it)
		{
			if((*it) != player)
				(*it)->onCreatureSay(npc, SPEAK_SAY, text);
		}
	}
	return true;
}

//--
bool Game::canThrowObjectTo(const Position& fromPos, const Position& toPos, bool checkLineOfSight /*= true*/,
	int32_t rangex /*= Map::maxClientViewportX*/, int32_t rangey /*= Map::maxClientViewportY*/)
{
	return map->canThrowObjectTo(fromPos, toPos, checkLineOfSight, rangex, rangey);
}

bool Game::isSightClear(const Position& fromPos, const Position& toPos, bool floorCheck)
{
	return map->isSightClear(fromPos, toPos, floorCheck);
}

bool Game::internalCreatureTurn(Creature* creature, Direction dir)
{
	if(creature->getDirection() != dir)
	{
		creature->setDirection(dir);

		int32_t stackpos = creature->getParent()->__getIndexOfThing(creature);

		const SpectatorVec& list = getSpectators(creature->getPosition());
		SpectatorVec::const_iterator it;

		//send to client
		Player* tmpPlayer = NULL;
		for(it = list.begin(); it != list.end(); ++it)
		{
			if((tmpPlayer = (*it)->getPlayer()))
				tmpPlayer->sendCreatureTurn(creature);
		}

		//event method
		for(it = list.begin(); it != list.end(); ++it)
			(*it)->onCreatureTurn(creature, stackpos);

		return true;
	}
	return false;
}

bool Game::internalCreatureSay(Creature* creature, SpeakClasses type, const std::string& text,
	bool ghostMode, SpectatorVec* listPtr/* = NULL*/, Position* pos/* = NULL*/)
{
	if(g_config.getBoolean(ConfigManager::ACCOUNT_MANAGER))
	{
		Player* player = creature->getPlayer();
		if(player && player->isAccountManager())
		{
			player->manageAccount(text);
			return true;
		}
	}

	Position destPos = creature->getPosition();
	if(pos)
		destPos = (*pos);

	SpectatorVec list;
	SpectatorVec::const_iterator it;
	if(!listPtr || !listPtr->size())
	{
		// This somewhat complex construct ensures that the cached SpectatorVec
		// is used if available and if it can be used, else a local vector is
		// used (hopefully the compiler will optimize away the construction of
		// the temporary when it's not used).
		if(type != SPEAK_YELL && type != SPEAK_MONSTER_YELL)
		{
			 getSpectators(list, destPos, false, false,
				Map::maxClientViewportX, Map::maxClientViewportX,
				Map::maxClientViewportY, Map::maxClientViewportY);
		}
		else
			getSpectators(list, destPos, false, true, 18, 18, 14, 14);
	}
	else
		list = (*listPtr);

	//send to client
	Player* tmpPlayer = NULL;
	for(it = list.begin(); it != list.end(); ++it)
	{
		if(!(tmpPlayer = (*it)->getPlayer()))
			continue;

		if(!ghostMode || tmpPlayer->canSeeCreature(creature))
			tmpPlayer->sendCreatureSay(creature, type, text, &destPos);
	}

	//event method
	for(it = list.begin(); it != list.end(); ++it)
		(*it)->onCreatureSay(creature, type, text, &destPos);

	return true;
}

bool Game::getPathTo(const Creature* creature, const Position& destPos,
	std::list<Direction>& listDir, int32_t maxSearchDist /*= -1*/)
{
	return map->getPathTo(creature, destPos, listDir, maxSearchDist);
}

bool Game::getPathToEx(const Creature* creature, const Position& targetPos,
	std::list<Direction>& dirList, const FindPathParams& fpp)
{
	return map->getPathMatching(creature, dirList, FrozenPathingConditionCall(targetPos), fpp);
}

bool Game::getPathToEx(const Creature* creature, const Position& targetPos, std::list<Direction>& dirList,
	uint32_t minTargetDist, uint32_t maxTargetDist, bool fullPathSearch /*= true*/,
	bool clearSight /*= true*/, int32_t maxSearchDist /*= -1*/)
{
	FindPathParams fpp;
	fpp.fullPathSearch = fullPathSearch;
	fpp.maxSearchDist = maxSearchDist;
	fpp.clearSight = clearSight;
	fpp.minTargetDist = minTargetDist;
	fpp.maxTargetDist = maxTargetDist;
	return getPathToEx(creature, targetPos, dirList, fpp);
}

void Game::checkCreatureWalk(uint32_t creatureId)
{
	Creature* creature = getCreatureByID(creatureId);
	if(creature && creature->getHealth() > 0)
	{
		creature->onWalk();
		cleanup();
	}
}

void Game::updateCreatureWalk(uint32_t creatureId)
{
	Creature* creature = getCreatureByID(creatureId);
	if(creature && creature->getHealth() > 0)
		creature->goToFollowCreature();
}

void Game::checkCreatureAttack(uint32_t creatureId)
{
	Creature* creature = getCreatureByID(creatureId);
	if(creature && creature->getHealth() > 0)
		creature->onAttacking(0);
}

void Game::addCreatureCheck(Creature* creature)
{
	if(creature->isRemoved())
		return;

	creature->creatureCheck = true;
	if(creature->checkCreatureVectorIndex >= 0)
		return; //Already in a vector

	toAddCheckCreatureVector.push_back(creature);
	creature->checkCreatureVectorIndex = random_range(0, EVENT_CREATURECOUNT - 1);
	creature->useThing2();
}

void Game::removeCreatureCheck(Creature* creature)
{
	if(creature->checkCreatureVectorIndex == -1)
		return;

	creature->creatureCheck = false;
}

void Game::checkCreatures()
{
	checkCreatureEvent = g_scheduler.addEvent(createSchedulerTask(EVENT_CHECK_CREATURE_INTERVAL, boost::bind(&Game::checkCreatures, this)));

	Creature* creature;
	std::vector<Creature*>::iterator it;

	//add any new creatures
	for(it = toAddCheckCreatureVector.begin(); it != toAddCheckCreatureVector.end(); ++it)
	{
		creature = (*it);
		checkCreatureVectors[creature->checkCreatureVectorIndex].push_back(creature);
	}
	toAddCheckCreatureVector.clear();

	checkCreatureLastIndex++;
	if(checkCreatureLastIndex == EVENT_CREATURECOUNT)
		checkCreatureLastIndex = 0;

	CreatureVector& checkCreatureVector = checkCreatureVectors[checkCreatureLastIndex];
	for(it = checkCreatureVector.begin(); it != checkCreatureVector.end();)
	{
		creature = (*it);
		if(creature->creatureCheck)
		{
			if(creature->getHealth() > 0)
			{
				creature->onThink(EVENT_CREATURE_THINK_INTERVAL);
				creature->onAttacking(EVENT_CREATURE_THINK_INTERVAL);
				creature->executeConditions(EVENT_CREATURE_THINK_INTERVAL);
			}
			else
				creature->onDeath();

			++it;
		}
		else
		{
			creature->checkCreatureVectorIndex = -1;
			it = checkCreatureVector.erase(it);
			FreeThing(creature);
		}
	}
	cleanup();
}

void Game::changeSpeed(Creature* creature, int32_t varSpeedDelta)
{
	int32_t varSpeed = creature->getSpeed() - creature->getBaseSpeed();
	varSpeed += varSpeedDelta;

	creature->setSpeed(varSpeed);

	const SpectatorVec& list = getSpectators(creature->getPosition());
	SpectatorVec::const_iterator it;

	//send to client
	Player* tmpPlayer = NULL;
	for(it = list.begin(); it != list.end(); ++it)
	{
		if((tmpPlayer = (*it)->getPlayer()))
			tmpPlayer->sendChangeSpeed(creature, creature->getStepSpeed());
	}
}

void Game::internalCreatureChangeOutfit(Creature* creature, const Outfit_t& outfit)
{
	creature->setCurrentOutfit(outfit);
	if(!creature->isInvisible())
	{
		const SpectatorVec& list = getSpectators(creature->getPosition());
		SpectatorVec::const_iterator it;

		//send to client
		Player* tmpPlayer = NULL;
		for(it = list.begin(); it != list.end(); ++it)
		{
			if((tmpPlayer = (*it)->getPlayer()))
				tmpPlayer->sendCreatureChangeOutfit(creature, outfit);
		}

		//event method
		for(it = list.begin(); it != list.end(); ++it)
			(*it)->onCreatureChangeOutfit(creature, outfit);
	}
}

void Game::internalCreatureChangeVisible(Creature* creature, bool visible)
{
	const SpectatorVec& list = getSpectators(creature->getPosition());
	SpectatorVec::const_iterator it;

	//send to client
	Player* tmpPlayer = NULL;
	for(it = list.begin(); it != list.end(); ++it)
	{
		if((tmpPlayer = (*it)->getPlayer()))
			tmpPlayer->sendCreatureChangeVisible(creature, visible);
	}

	//event method
	for(it = list.begin(); it != list.end(); ++it)
		(*it)->onCreatureChangeVisible(creature, visible);
}


void Game::changeLight(const Creature* creature)
{
	const SpectatorVec& list = getSpectators(creature->getPosition());

	//send to client
	Player* tmpPlayer = NULL;
	for(SpectatorVec::const_iterator it = list.begin(); it != list.end(); ++it)
	{
		if((tmpPlayer = (*it)->getPlayer()))
			tmpPlayer->sendCreatureLight(creature);
	}
}

bool Game::combatBlockHit(CombatType_t combatType, Creature* attacker, Creature* target,
	int32_t& healthChange, bool checkDefense, bool checkArmor)
{
	if(target->getPlayer() && target->getPlayer()->isInGhostMode())
		return true;

	if(healthChange > 0)
		return false;

	const Position& targetPos = target->getPosition();
	const SpectatorVec& list = getSpectators(targetPos);

	if(!target->isAttackable() || Combat::canDoCombat(attacker, target) != RET_NOERROR)
	{
		addMagicEffect(list, targetPos, NM_ME_POFF, target->isInGhostMode());
		return true;
	}

	int32_t damage = -healthChange;
	BlockType_t blockType = target->blockHit(attacker, combatType, damage, checkDefense, checkArmor);
	healthChange = -damage;

	if(blockType == BLOCK_DEFENSE)
	{
		addMagicEffect(list, targetPos, NM_ME_POFF);
		return true;
	}
	else if(blockType == BLOCK_ARMOR)
	{
		addMagicEffect(list, targetPos, NM_ME_BLOCKHIT);
		return true;
	}
	else if(blockType == BLOCK_IMMUNITY)
	{
		uint8_t hitEffect = 0;
		switch(combatType)
		{
			case COMBAT_UNDEFINEDDAMAGE:
				break;

			case COMBAT_ENERGYDAMAGE:
			case COMBAT_FIREDAMAGE:
			case COMBAT_PHYSICALDAMAGE:
			case COMBAT_ICEDAMAGE:
			case COMBAT_DEATHDAMAGE:
			{
				hitEffect = NM_ME_BLOCKHIT;
				break;
			}

			case COMBAT_EARTHDAMAGE:
			{
				hitEffect = NM_ME_POISON_RINGS;
				break;
			}

			case COMBAT_HOLYDAMAGE:
			{
				hitEffect = NM_ME_HOLYDAMAGE;
				break;
			}

			default:
			{
				hitEffect = NM_ME_POFF;
				break;
			}
		}
		addMagicEffect(list, targetPos, hitEffect);
		return true;
	}
	return false;
}

bool Game::combatChangeHealth(CombatType_t combatType, Creature* attacker, Creature* target, int32_t healthChange)
{
	const Position& targetPos = target->getPosition();
	if(healthChange > 0)
	{
		if(target->getHealth() <= 0)
			return false;

		Player* attackerPlayer = NULL;
		if(attacker)
			attackerPlayer = attacker->getPlayer();

		Player* targetPlayer = target->getPlayer();
		if(attackerPlayer && targetPlayer)
		{
			if(g_config.getBoolean(ConfigManager::CANNOT_ATTACK_SAME_LOOKFEET) && attacker->defaultOutfit.lookFeet == target->defaultOutfit.lookFeet && combatType != COMBAT_HEALING)
				return false;

			if(attackerPlayer->getSkull() == SKULL_BLACK && attackerPlayer->getSkullClient(targetPlayer) == SKULL_NONE)
				return false;
		}

		int32_t realHealthChange = target->getHealth();
		target->changeHealth(healthChange);
		realHealthChange = target->getHealth() - realHealthChange;
		if(realHealthChange > 0 && !target->isInGhostMode())
		{
			std::ostringstream ss;
			if(!attacker)
				ss << ucfirst(target->getNameDescription()) << " was healed for " << realHealthChange << " hitpoint" << (realHealthChange != 1 ? "s." : ".");
			else if(attacker == target)
				ss << ucfirst(attacker->getNameDescription()) << " healed " << (targetPlayer ? (targetPlayer->getSex() == PLAYERSEX_FEMALE ? "herself for " : "himself for ") : "itself for ") << realHealthChange << " hitpoint" << (realHealthChange != 1 ? "s." : ".");
			else
				ss << ucfirst(attacker->getNameDescription()) << " healed " << target->getNameDescription() << " for " << realHealthChange << " hitpoint" << (realHealthChange != 1 ? "s." : ".");

			std::string message = ss.str();

			Player* tmpPlayer = NULL;
			SpectatorVec list;
			getSpectators(list, targetPos);
			for(SpectatorVec::const_iterator it = list.begin(), end = list.end(); it != end; ++it)
			{
				if((tmpPlayer = (*it)->getPlayer()))
				{
					if(tmpPlayer == attackerPlayer && attackerPlayer != targetPlayer)
					{
						std::ostringstream tmpSs;
						tmpSs << "You heal " << target->getNameDescription() << " for " << realHealthChange << " hitpoint" << (realHealthChange != 1 ? "s." : ".");
						tmpPlayer->sendHealMessage(MSG_HEALED, tmpSs.str(), targetPos, realHealthChange, TEXTCOLOR_MAYABLUE);
					}
					else if(tmpPlayer == targetPlayer)
					{
						std::ostringstream tmpSs;
						if(!attacker)
							tmpSs << "You were healed for " << realHealthChange << " hitpoint" << (realHealthChange != 1 ? "s." : ".");
						else if(targetPlayer == attackerPlayer)
							tmpSs << "You heal yourself for " << realHealthChange << " hitpoint" << (realHealthChange != 1 ? "s." : ".");
						else
							tmpSs << "You were healed by " << attacker->getNameDescription() << " for " << realHealthChange << " hitpoint" << (realHealthChange != 1 ? "s." : ".");

						tmpPlayer->sendHealMessage(MSG_HEALED, tmpSs.str(), targetPos, realHealthChange, TEXTCOLOR_MAYABLUE);
					}
					else
						tmpPlayer->sendHealMessage(MSG_HEALED_OTHERS, message, targetPos, realHealthChange, TEXTCOLOR_MAYABLUE);
				}
			}
		}
	}
	else
	{
		const SpectatorVec& list = getSpectators(targetPos);
		if(!target->isAttackable() || Combat::canDoCombat(attacker, target) != RET_NOERROR)
		{
			addMagicEffect(list, targetPos, NM_ME_POFF);
			return true;
		}

		Player* attackerPlayer = NULL;
		if(attacker)
			attackerPlayer = attacker->getPlayer();

		Player* targetPlayer = target->getPlayer();
		if(attackerPlayer && targetPlayer)
		{
			if(g_config.getBoolean(ConfigManager::CANNOT_ATTACK_SAME_LOOKFEET) && attacker->defaultOutfit.lookFeet == target->defaultOutfit.lookFeet && combatType != COMBAT_HEALING)
				return false;

			if(attackerPlayer->getSkull() == SKULL_BLACK && attackerPlayer->getSkullClient(targetPlayer) == SKULL_NONE)
				return false;
		}

		int32_t damage = -healthChange;
		if(damage != 0)
		{
			if(target->hasCondition(CONDITION_MANASHIELD) && combatType != COMBAT_UNDEFINEDDAMAGE)
			{
				int32_t manaDamage = std::min(target->getMana(), damage);
				damage = std::max((int32_t)0, damage - manaDamage);
				if(manaDamage != 0)
				{
					target->drainMana(attacker, manaDamage);
					addMagicEffect(list, targetPos, NM_ME_LOSE_ENERGY);

					std::ostringstream ss;
					if(!attacker)
						ss << ucfirst(target->getNameDescription()) << " loses " << manaDamage << " mana.";
					else if(attacker == target)
						ss << ucfirst(target->getNameDescription()) << " loses " << manaDamage << " mana blocking an attack by " << (targetPlayer ? (targetPlayer->getSex() == PLAYERSEX_FEMALE ? "herself." : "himself.") : "itself.");
					else
						ss << ucfirst(target->getNameDescription()) << " loses " << manaDamage << " mana blocking an attack by " << attacker->getNameDescription() << ".";

					std::string message = ss.str();

					Player* tmpPlayer = NULL;
					SpectatorVec listEx;
					getSpectators(listEx, targetPos);
					for(SpectatorVec::const_iterator it = listEx.begin(), end = listEx.end(); it != end; ++it)
					{
						if((tmpPlayer = (*it)->getPlayer()))
						{
							if(tmpPlayer == attackerPlayer && attackerPlayer != targetPlayer)
							{
								std::ostringstream tmpSs;
								tmpSs << ucfirst(target->getNameDescription()) << " loses " << manaDamage << " mana blocking your attack.";
								tmpPlayer->sendDamageMessage(MSG_DAMAGE_DEALT, tmpSs.str(), targetPos, manaDamage, TEXTCOLOR_BLUE);
							}
							else if(tmpPlayer == targetPlayer)
							{
								std::ostringstream tmpSs;
								if(!attacker)
									tmpSs << "You lose " << manaDamage << " mana.";
								else if(targetPlayer == attackerPlayer)
									tmpSs << "You lose " << manaDamage << " mana blocking an attack by yourself.";
								else
									tmpSs << "You lose " << manaDamage << " mana blocking an attack by " << attacker->getNameDescription() << ".";

								tmpPlayer->sendDamageMessage(MSG_DAMAGE_RECEIVED, tmpSs.str(), targetPos, manaDamage, TEXTCOLOR_BLUE);
							}
							else
								tmpPlayer->sendDamageMessage(MSG_DAMAGE_OTHERS, message, targetPos, manaDamage, TEXTCOLOR_BLUE);
						}
					}
				}
			}

			Player* targetPlayer = target->getPlayer();
			if(targetPlayer)
			{
				if(damage >= targetPlayer->getHealth())
				{
					//scripting event - onPrepareDeath
					CreatureEventList prepareDeathEvents = targetPlayer->getCreatureEvents(CREATURE_EVENT_PREPAREDEATH);
					for(CreatureEventList::const_iterator it = prepareDeathEvents.begin(); it != prepareDeathEvents.end(); ++it)
						(*it)->executeOnPrepareDeath(targetPlayer, attacker);
				}
			}

			damage = std::min(target->getHealth(), damage);
			if(damage > 0)
			{
				target->drainHealth(attacker, combatType, damage);
				addCreatureHealth(list, target);

				TextColor_t textColor = TEXTCOLOR_NONE;
				uint8_t hitEffect = 0;
				switch(combatType)
				{
					case COMBAT_PHYSICALDAMAGE:
					{
						Item* splash = NULL;
						switch(target->getRace())
						{
							case RACE_VENOM:
								textColor = TEXTCOLOR_LIGHTGREEN;
								hitEffect = NM_ME_POISON;
								splash = Item::CreateItem(ITEM_SMALLSPLASH, FLUID_GREEN);
								break;

							case RACE_BLOOD:
								textColor = TEXTCOLOR_RED;
								hitEffect = NM_ME_DRAW_BLOOD;
								splash = Item::CreateItem(ITEM_SMALLSPLASH, FLUID_BLOOD);
								break;

							case RACE_UNDEAD:
								textColor = TEXTCOLOR_LIGHTGREY;
								hitEffect = NM_ME_HIT_AREA;
								break;

							case RACE_FIRE:
								textColor = TEXTCOLOR_ORANGE;
								hitEffect = NM_ME_DRAW_BLOOD;
								break;

							case RACE_ENERGY:
								textColor = TEXTCOLOR_PURPLE;
								hitEffect = NM_ME_ENERGY_DAMAGE;
								break;

							default:
								break;
						}

						if(splash)
						{
							internalAddItem(target->getTile(), splash, INDEX_WHEREEVER, FLAG_NOLIMIT);
							startDecay(splash);
						}
						break;
					}

					case COMBAT_ENERGYDAMAGE:
					{
						textColor = TEXTCOLOR_PURPLE;
						hitEffect = NM_ME_ENERGY_DAMAGE;
						break;
					}

					case COMBAT_EARTHDAMAGE:
					{
						textColor = TEXTCOLOR_LIGHTGREEN;
						hitEffect = NM_ME_POISON_RINGS;
						break;
					}

					case COMBAT_DROWNDAMAGE:
					{
						textColor = TEXTCOLOR_LIGHTBLUE;
						hitEffect = NM_ME_LOSE_ENERGY;
						break;
					}

					case COMBAT_FIREDAMAGE:
					{
						textColor = TEXTCOLOR_ORANGE;
						hitEffect = NM_ME_HITBY_FIRE;
						break;
					}

					case COMBAT_ICEDAMAGE:
					{
						textColor = TEXTCOLOR_SKYBLUE;
						hitEffect = NM_ME_ICEATTACK;
						break;
					}

					case COMBAT_HOLYDAMAGE:
					{
						textColor = TEXTCOLOR_YELLOW;
						hitEffect = NM_ME_HOLYDAMAGE;
						break;
					}

					case COMBAT_DEATHDAMAGE:
					{
						textColor = TEXTCOLOR_DARKRED;
						hitEffect = NM_ME_SMALLCLOUDS;
						break;
					}

					case COMBAT_LIFEDRAIN:
					{
						textColor = TEXTCOLOR_RED;
						hitEffect = NM_ME_MAGIC_BLOOD;
						break;
					}

					default:
						break;
				}

				if(textColor != TEXTCOLOR_NONE)
				{
					addMagicEffect(list, targetPos, hitEffect);

					std::ostringstream ss;
					if(!attacker)
						ss << ucfirst(target->getNameDescription()) << " loses " << damage << " hitpoint" << (damage != 1 ? "s." : ".");
					else if(attacker == target)
						ss << ucfirst(target->getNameDescription()) << " loses " << damage << " hitpoint" << (damage != 1 ? "s" : "") << " due to " << (targetPlayer ? (targetPlayer->getSex() == PLAYERSEX_FEMALE ? "her" : "his") : "its") << " own attack.";
					else
						ss << ucfirst(target->getNameDescription()) << " loses " << damage << " hitpoint" << (damage != 1 ? "s" : "") << " due to an attack by " << attacker->getNameDescription() << ".";

					std::string message = ss.str();

					Player* tmpPlayer = NULL;
					SpectatorVec listEx;
					getSpectators(listEx, targetPos);
					for(SpectatorVec::const_iterator it = listEx.begin(), end = listEx.end(); it != end; ++it)
					{
						if((tmpPlayer = (*it)->getPlayer()))
						{
							if(tmpPlayer == attackerPlayer && attackerPlayer != targetPlayer)
							{
								std::ostringstream tmpSs;
								tmpSs << ucfirst(target->getNameDescription()) << " loses " << damage << " hitpoint" << (damage != 1 ? "s" : "") << " due to your attack.";
								tmpPlayer->sendDamageMessage(MSG_DAMAGE_DEALT, tmpSs.str(), targetPos, damage, textColor);
							}
							else if(tmpPlayer == targetPlayer)
							{
								std::ostringstream tmpSs;
								if(!attacker)
									tmpSs << "You lose " << damage << " hitpoint" << (damage != 1 ? "s." : ".");
								else if(targetPlayer == attackerPlayer)
									tmpSs << "You lose " << damage << " hitpoint" << (damage != 1 ? "s" : "") << " due to your own attack.";
								else
									tmpSs << "You lose " << damage << " hitpoint" << (damage != 1 ? "s" : "") << " due to an attack by " << attacker->getNameDescription() << ".";

								tmpPlayer->sendDamageMessage(MSG_DAMAGE_RECEIVED, tmpSs.str(), targetPos, damage, textColor);
							}
							else
								tmpPlayer->sendDamageMessage(MSG_DAMAGE_OTHERS, message, targetPos, damage, textColor);
						}
					}
				}
			}
		}
	}
	return true;
}

bool Game::combatChangeMana(Creature* attacker, Creature* target, int32_t manaChange)
{
	if(manaChange > 0)
	{
		if(attacker)
		{
			Player* attackerPlayer = attacker->getPlayer();
			Player* targetPlayer = target->getPlayer();
			if(attackerPlayer && targetPlayer)
			{
				if(g_config.getBoolean(ConfigManager::CANNOT_ATTACK_SAME_LOOKFEET) && attacker->defaultOutfit.lookFeet == target->defaultOutfit.lookFeet)
					return false;

				if(attackerPlayer->getSkull() == SKULL_BLACK && attackerPlayer->getSkullClient(targetPlayer) == SKULL_NONE)
					return false;
			}
		}
		target->changeMana(manaChange);
	}
	else
	{
		const Position& targetPos = target->getPosition();
		if(!target->isAttackable() || Combat::canDoCombat(attacker, target) != RET_NOERROR)
		{
			addMagicEffect(getSpectators(targetPos), targetPos, NM_ME_POFF);
			return false;
		}

		Player* attackerPlayer = NULL;
		if(attacker)
			attackerPlayer = attacker->getPlayer();

		Player* targetPlayer = target->getPlayer();
		if(attackerPlayer && targetPlayer)
		{
			if(g_config.getBoolean(ConfigManager::CANNOT_ATTACK_SAME_LOOKFEET) && attacker->defaultOutfit.lookFeet == target->defaultOutfit.lookFeet)
				return false;

			if(attackerPlayer->getSkull() == SKULL_BLACK && attackerPlayer->getSkullClient(targetPlayer) == SKULL_NONE)
				return false;
		}

		int32_t manaLoss = std::min(target->getMana(), -manaChange);
		BlockType_t blockType = target->blockHit(attacker, COMBAT_MANADRAIN, manaLoss);

		if(blockType != BLOCK_NONE)
		{
			addMagicEffect(getSpectators(targetPos), targetPos, NM_ME_POFF);
			return false;
		}

		if(manaLoss > 0)
		{
			target->drainMana(attacker, manaLoss);

			std::ostringstream ss;
			if(!attacker)
				ss << ucfirst(target->getNameDescription()) << " loses " << manaLoss << " mana.";
			else if(attacker == target)
				ss << ucfirst(target->getNameDescription()) << " loses " << manaLoss << " mana blocking an attack by " << (targetPlayer ? (targetPlayer->getSex() == PLAYERSEX_FEMALE ? "herself." : "himself.") : "itself.");
			else
				ss << ucfirst(target->getNameDescription()) << " loses " << manaLoss << " mana blocking an attack by " << attacker->getNameDescription() << ".";

			std::string message = ss.str();

			Player* tmpPlayer = NULL;
			SpectatorVec list;
			getSpectators(list, targetPos);
			for(SpectatorVec::const_iterator it = list.begin(), end = list.end(); it != end; ++it)
			{
				if((tmpPlayer = (*it)->getPlayer()))
				{
					if(tmpPlayer == attackerPlayer && attackerPlayer != targetPlayer)
					{
						std::ostringstream tmpSs;
						tmpSs << ucfirst(target->getNameDescription()) << " loses " << manaLoss << " mana blocking your attack.";
						tmpPlayer->sendDamageMessage(MSG_DAMAGE_DEALT, tmpSs.str(), targetPos, manaLoss, TEXTCOLOR_BLUE);
					}
					else if(tmpPlayer == targetPlayer)
					{
						std::ostringstream tmpSs;
						if(!attacker)
							tmpSs << "You lose " << manaLoss << " mana.";
						else if(targetPlayer == attackerPlayer)
							tmpSs << "You lose " << manaLoss << " mana blocking an attack by yourself.";
						else
							tmpSs << "You lose " << manaLoss << " mana blocking an attack by " << attacker->getNameDescription() << ".";

						tmpPlayer->sendDamageMessage(MSG_DAMAGE_RECEIVED, tmpSs.str(), targetPos, manaLoss, TEXTCOLOR_BLUE);
					}
					else
						tmpPlayer->sendDamageMessage(MSG_DAMAGE_OTHERS, message, targetPos, manaLoss, TEXTCOLOR_BLUE);
				}
			}
		}
	}
	return true;
}

void Game::addCreatureHealth(const Creature* target)
{
	const SpectatorVec& list = getSpectators(target->getPosition());
	addCreatureHealth(list, target);
}

void Game::addCreatureHealth(const SpectatorVec& list, const Creature* target)
{
	Player* player = NULL;
	for(SpectatorVec::const_iterator it = list.begin(); it != list.end(); ++it)
	{
		if((player = (*it)->getPlayer()))
			player->sendCreatureHealth(target);
	}
}

void Game::addMagicEffect(const Position& pos, uint8_t effect, bool ghostMode /* = false */)
{
	if(ghostMode)
		return;

	const SpectatorVec& list = getSpectators(pos);
	addMagicEffect(list, pos, effect);
}

void Game::addMagicEffect(const SpectatorVec& list, const Position& pos, uint8_t effect, bool ghostMode /* = false */)
{
	if(ghostMode)
		return;

	Player* player = NULL;
	for(SpectatorVec::const_iterator it = list.begin(); it != list.end(); ++it)
	{
		if((player = (*it)->getPlayer()))
			player->sendMagicEffect(pos, effect);
	}
}

void Game::addDistanceEffect(const Position& fromPos, const Position& toPos,
	uint8_t effect)
{
	SpectatorVec list;

	getSpectators(list, fromPos, false, true);
	getSpectators(list, toPos, true, true);

	Player* player = NULL;
	for(SpectatorVec::const_iterator it = list.begin(); it != list.end(); ++it)
	{
		if((player = (*it)->getPlayer()))
			player->sendDistanceShoot(fromPos, toPos, effect);
	}
}

void Game::startDecay(Item* item)
{
	if(item && item->canDecay())
	{
		uint32_t decayState = item->getDecaying();
		if(decayState == DECAYING_TRUE)
			 return;

		if(item->getDuration() > 0)
		{
			item->useThing2();
			item->setDecaying(DECAYING_TRUE);
			toDecayItems.push_back(item);
		}
		else
			internalDecayItem(item);
	}
}

void Game::internalDecayItem(Item* item)
{
	const ItemType& it = Item::items[item->getID()];
	if(it.decayTo != 0)
	{
		Item* newItem = transformItem(item, it.decayTo);
		startDecay(newItem);
	}
	else
	{
		ReturnValue ret = internalRemoveItem(item);
		if(ret != RET_NOERROR)
			std::cout << "DEBUG, internalDecayItem failed, error code: " << (int32_t) ret << "item id: " << item->getID() << std::endl;
	}
}

void Game::checkDecay()
{
	checkDecayEvent = g_scheduler.addEvent(createSchedulerTask(EVENT_DECAYINTERVAL,
		boost::bind(&Game::checkDecay, this)));

	size_t bucket = (lastBucket + 1) % EVENT_DECAY_BUCKETS;
	for(DecayList::iterator it = decayItems[bucket].begin(); it != decayItems[bucket].end();)
	{
		Item* item = *it;
		if(!item->canDecay())
		{
			item->setDecaying(DECAYING_FALSE);
			FreeThing(item);
			it = decayItems[bucket].erase(it);
			continue;
		}

		int32_t decreaseTime = EVENT_DECAYINTERVAL * EVENT_DECAY_BUCKETS;
		if((int32_t)item->getDuration() - decreaseTime < 0)
			decreaseTime = item->getDuration();

		item->decreaseDuration(decreaseTime);

		int32_t dur = item->getDuration();
		if(dur <= 0)
		{
			it = decayItems[bucket].erase(it);
			internalDecayItem(item);
			FreeThing(item);
		}
		else if(dur < EVENT_DECAYINTERVAL * EVENT_DECAY_BUCKETS)
		{
			it = decayItems[bucket].erase(it);
			size_t newBucket = (bucket + ((dur + EVENT_DECAYINTERVAL / 2) / 1000)) % EVENT_DECAY_BUCKETS;
			if(newBucket == bucket)
			{
				internalDecayItem(item);
				FreeThing(item);
			}
			else
				decayItems[newBucket].push_back(item);
		}
		else
			++it;
	}

	lastBucket = bucket;
	cleanup();
}

void Game::checkLight()
{
	checkLightEvent = g_scheduler.addEvent(createSchedulerTask(EVENT_LIGHTINTERVAL,
		boost::bind(&Game::checkLight, this)));

	lightHour += lightHourDelta;
	if(lightHour > 1440)
		lightHour -= 1440;

	if(std::abs(lightHour - SUNRISE) < 2 * lightHourDelta)
		lightState = LIGHT_STATE_SUNRISE;
	else if(std::abs(lightHour - SUNSET) < 2 * lightHourDelta)
		lightState = LIGHT_STATE_SUNSET;

	int32_t newLightLevel = lightLevel;
	bool lightChange = false;
	switch(lightState)
	{
		case LIGHT_STATE_SUNRISE:
		{
			newLightLevel += (LIGHT_LEVEL_DAY - LIGHT_LEVEL_NIGHT) / 30;
			lightChange = true;
			break;
		}
		case LIGHT_STATE_SUNSET:
		{
			newLightLevel -= (LIGHT_LEVEL_DAY - LIGHT_LEVEL_NIGHT) / 30;
			lightChange = true;
			break;
		}
		default:
			break;
	}

	if(newLightLevel <= LIGHT_LEVEL_NIGHT)
	{
		lightLevel = LIGHT_LEVEL_NIGHT;
		lightState = LIGHT_STATE_NIGHT;
	}
	else if(newLightLevel >= LIGHT_LEVEL_DAY)
	{
		lightLevel = LIGHT_LEVEL_DAY;
		lightState = LIGHT_STATE_DAY;
	}
	else
		lightLevel = newLightLevel;

	if(lightChange)
	{
		LightInfo lightInfo;
		getWorldLightInfo(lightInfo);
		for(AutoList<Player>::listiterator it = Player::listPlayer.list.begin(); it != Player::listPlayer.list.end(); ++it)
			(*it).second->sendWorldLight(lightInfo);
	}
}

void Game::getWorldLightInfo(LightInfo& lightInfo) const
{
	lightInfo.level = lightLevel;
	lightInfo.color = 0xD7;
}

void Game::addCommandTag(std::string tag)
{
	bool found = false;
	for(uint32_t i=0; i< commandTags.size() ;i++)
	{
		if(commandTags[i] == tag)
		{
			found = true;
			break;
		}
	}

	if(!found)
		commandTags.push_back(tag);
}

void Game::resetCommandTag()
{
	commandTags.clear();
}

void Game::shutdown()
{
	std::cout << "Shutting down server..." << std::flush;

	g_scheduler.shutdown();
	g_dispatcher.shutdown();
	Spawns::getInstance()->clear();
	Raids::getInstance()->clear();

	cleanup();

	if(services)
		services->stop();

	std::cout << " done!";
}

void Game::cleanup()
{
	//free memory
	for(std::vector<Thing*>::iterator it = ToReleaseThings.begin(); it != ToReleaseThings.end(); ++it)
		(*it)->releaseThing2();

	ToReleaseThings.clear();
	for(DecayList::iterator it = toDecayItems.begin(); it != toDecayItems.end(); ++it)
	{
		int32_t dur = (*it)->getDuration();
		if(dur >= EVENT_DECAYINTERVAL * EVENT_DECAY_BUCKETS)
			decayItems[lastBucket].push_back(*it);
		else
			decayItems[(lastBucket + 1 + (*it)->getDuration() / 1000) % EVENT_DECAY_BUCKETS].push_back(*it);
	}

	toDecayItems.clear();
}

void Game::FreeThing(Thing* thing)
{
	ToReleaseThings.push_back(thing);
}

bool Game::reloadHighscores()
{
	lastHSUpdate = time(NULL);
	for(int16_t i = 0; i <= 8; i++)
		highscoreStorage[i] = getHighscore(i);
	return true;
}

void Game::timedHighscoreUpdate()
{
	int32_t highscoreUpdateTime = g_config.getNumber(ConfigManager::HIGHSCORES_UPDATETIME) * 60 * 1000;
	if(highscoreUpdateTime <= 0)
		return;

	reloadHighscores();
	g_scheduler.addEvent(createSchedulerTask(highscoreUpdateTime, boost::bind(&Game::timedHighscoreUpdate, this)));
}

std::string Game::getHighscoreString(uint16_t skill)
{
	Highscore hs = highscoreStorage[skill];
	std::ostringstream ss;
	ss << "Highscore for " << getSkillName(skill) << "\n\nRank Level - Player Name";
	for(uint32_t i = 0; i < hs.size(); ++i)
		ss << "\n" << (i + 1) << ".  " << hs[i].second << "  -  " << hs[i].first;

	ss << "\n\nLast updated on:\n" << std::ctime(&lastHSUpdate);
	std::string highscoresStr = ss.str();
	highscoresStr.erase(highscoresStr.length() - 1);
	return highscoresStr;
}

bool Game::broadcastMessage(const std::string& text, MessageClasses type)
{
	std::cout << "> Broadcasted message: \"" << text << "\"." << std::endl;
	for(AutoList<Player>::listiterator it = Player::listPlayer.list.begin(); it != Player::listPlayer.list.end(); ++it)
		(*it).second->sendTextMessage(type, text);
	return true;
}

Highscore Game::getHighscore(uint16_t skill)
{
	Highscore hs;
	Database* db = Database::getInstance();

	DBQuery query;
	uint32_t highscoresTop = g_config.getNumber(ConfigManager::HIGHSCORES_TOP);
	if(skill > SKILL_LAST)
	{
		if(skill == SKILL__MAGLEVEL)
			query << "SELECT `name`, `maglevel` FROM `players` ORDER BY `maglevel` DESC, `manaspent` DESC LIMIT " << highscoresTop;
		else if(skill == SKILL__LEVEL)
			query << "SELECT `name`, `level` FROM `players` ORDER BY `level` DESC, `experience` DESC LIMIT " << highscoresTop;
		else
			return hs;

		DBResult* result;
		if((result = db->storeQuery(query.str())))
		{
			do
			{
				uint32_t level;
				if(skill == SKILL__MAGLEVEL)
					level = result->getDataInt("maglevel");
				else
					level = result->getDataInt("level");

				hs.push_back(make_pair(result->getDataString("name"), level));

				highscoresTop--;
				if(highscoresTop == 0)
					break;
			}
			while(result->next());
			db->freeResult(result);
		}
	}
	else
	{
		query << "SELECT * FROM `player_skills` WHERE `skillid`=" << skill << " ORDER BY `value` DESC, `count` DESC";

		DBResult* result;
		if((result = db->storeQuery(query.str())))
		{
			do
			{
				query.str("");
				query << "SELECT `name` FROM `players` WHERE `id` = " << result->getDataInt("player_id") << ";";

				DBResult* tmpResult;
				if((tmpResult = db->storeQuery(query.str())))
				{
					hs.push_back(make_pair(tmpResult->getDataString("name"), result->getDataInt("value")));
					db->freeResult(tmpResult);
				}

				highscoresTop--;
				if(highscoresTop == 0)
					break;
			}
			while(result->next());
			db->freeResult(result);
		}
	}
	return hs;
}

void Game::updateCreatureSkull(Player* player)
{
	if(getWorldType() != WORLD_TYPE_PVP)
		return;

	const SpectatorVec& list = getSpectators(player->getPosition());

	//send to client
	Player* tmpPlayer = NULL;
	for(SpectatorVec::const_iterator it = list.begin(); it != list.end(); ++it)
	{
		 if((tmpPlayer = (*it)->getPlayer()))
			tmpPlayer->sendCreatureSkull(player);
	}
}

void Game::updatePremium(Account& account)
{
	bool save = false;
	time_t timeNow = time(NULL);
	if(account.premiumDays != 0 && account.premiumDays != 0xFFFF)
	{
		if(account.lastDay == 0)
		{
			account.lastDay = timeNow;
			save = true;
		}
		else
		{
			uint32_t days = (timeNow - account.lastDay) / 86400;
			if(days > 0)
			{
				if(days >= account.premiumDays)
				{
					account.premiumDays = 0;
					account.lastDay = 0;
				}
				else
				{
					account.premiumDays -= days;
					uint32_t remainder = (timeNow - account.lastDay) % 86400;
					account.lastDay = timeNow - remainder;
				}
				save = true;
			}
		}
	}
	else if(account.lastDay != 0)
	{
		account.lastDay = 0;
		save = true;
	}

	if(save && !IOLoginData::getInstance()->saveAccount(account))
		std::cout << "> ERROR: Failed to save account: " << account.name << "!" << std::endl;
}

void Game::autoSave()
{
	int32_t autoSaveEachMinutes = g_config.getNumber(ConfigManager::AUTO_SAVE_EACH_MINUTES);
	if(autoSaveEachMinutes <= 0)
		return;

	g_dispatcher.addTask(createTask(boost::bind(&Game::saveGameState, this)));
	g_scheduler.addEvent(createSchedulerTask(autoSaveEachMinutes * 1000 * 60, boost::bind(&Game::autoSave, this)));
}

void Game::prepareServerSave()
{
	if(!serverSaveMessage[0])
	{
		serverSaveMessage[0] = true;
		broadcastMessage("Server is saving game in 5 minutes. Please logout.", MSG_STATUS_WARNING);
		g_scheduler.addEvent(createSchedulerTask(120000, boost::bind(&Game::prepareServerSave, this)));
	}
	else if(!serverSaveMessage[1])
	{
		serverSaveMessage[1] = true;
		broadcastMessage("Server is saving game in 3 minutes. Please logout.", MSG_STATUS_WARNING);
		g_scheduler.addEvent(createSchedulerTask(120000, boost::bind(&Game::prepareServerSave, this)));
	}
	else if(!serverSaveMessage[2])
	{
		serverSaveMessage[2] = true;
		broadcastMessage("Server is saving game in one minute. Please logout.", MSG_STATUS_WARNING);
		g_scheduler.addEvent(createSchedulerTask(60000, boost::bind(&Game::prepareServerSave, this)));
	}
	else
		serverSave();
}

void Game::serverSave()
{
	if(g_config.getBoolean(ConfigManager::SHUTDOWN_AT_SERVERSAVE))
	{
		//shutdown server
		setGameState(GAME_STATE_SHUTDOWN);
	}
	else
	{
		//close server
		setGameState(GAME_STATE_CLOSED);

		//clean map if configured to
		if(g_config.getBoolean(ConfigManager::CLEAN_MAP_AT_SERVERSAVE))
			map->clean();

		//reload highscores
		reloadHighscores();

		//reset variables
		for(int16_t i = 0; i < 3; i++)
			setServerSaveMessage(i, false);

		//prepare for next serversave after 24 hours
		g_scheduler.addEvent(createSchedulerTask(86100000, boost::bind(&Game::prepareServerSave, this)));

		//open server
		setGameState(GAME_STATE_NORMAL);
	}
}

Position Game::getClosestFreeTile(Player* player, Creature* teleportedCreature, Position toPos, bool teleport)
{
	int i;
	Tile* tile[9] =
	{
		getTile(toPos.x, toPos.y, toPos.z),
		getTile(toPos.x - 1, toPos.y - 1, toPos.z), getTile(toPos.x, toPos.y - 1, toPos.z),
		getTile(toPos.x + 1, toPos.y - 1, toPos.z), getTile(toPos.x - 1, toPos.y, toPos.z),
		getTile(toPos.x + 1, toPos.y, toPos.z), getTile(toPos.x - 1, toPos.y + 1, toPos.z),
		getTile(toPos.x, toPos.y + 1, toPos.z), getTile(toPos.x + 1, toPos.y + 1, toPos.z),
	};
	if(teleport)
	{
		if(player)
		{
			for(i = 0; i < 9; i++)
			{
				if(tile[i] && ((!tile[i]->hasProperty(IMMOVABLEBLOCKSOLID) && tile[i]->getCreatureCount() == 0) || player->getAccountType() == ACCOUNT_TYPE_GOD))
					return tile[i]->getPosition();
			}
		}
	}
	else if(teleportedCreature)
	{
		Player* teleportedPlayer = teleportedCreature->getPlayer();
		if(teleportedPlayer)
		{
			for(i = 0; i < 9; i++)
			{
				if(tile[i] && ((!tile[i]->hasProperty(IMMOVABLEBLOCKSOLID) && tile[i]->getCreatureCount() == 0) || teleportedPlayer->getAccountType() == ACCOUNT_TYPE_GOD))
					return tile[i]->getPosition();
			}
		}
		else
		{
			for(i = 0; i < 9; i++)
			{
				if(tile[i] && tile[i]->getCreatureCount() == 0 && !tile[i]->hasProperty(IMMOVABLEBLOCKSOLID))
					return tile[i]->getPosition();
			}
		}
	}
	return Position(0, 0, 0);
}

int32_t Game::getMotdNum()
{
	if(lastMotdText != g_config.getString(ConfigManager::MOTD))
	{
		lastMotdNum++;
		lastMotdText = g_config.getString(ConfigManager::MOTD);

		FILE* file = fopen("lastMotd.txt", "w");
		if(file != NULL)
		{
			fprintf(file, "%d", lastMotdNum);
			fprintf(file, "\n%s", lastMotdText.c_str());
			fclose(file);
		}
	}
	return lastMotdNum;
}

void Game::loadMotd()
{
	std::ifstream file("lastMotd.txt");
	if(!file)
	{
		std::cout << "> ERROR: Failed to load lastMotd.txt" << std::endl;
		lastMotdNum = random_range(5, 500);
		return;
	}

	std::string tmpStr;
	getline(file, tmpStr);
	getline(file, lastMotdText);
	lastMotdNum = atoi(tmpStr.c_str());
	file.close();
}

void Game::checkPlayersRecord()
{
	if(getPlayersOnline() > lastPlayersRecord)
	{
		uint32_t tmplPlayersRecord = lastPlayersRecord;
		lastPlayersRecord = getPlayersOnline();
		GlobalEventMap recordEvents = g_globalEvents->getEventMap(GLOBALEVENT_RECORD);
		for(GlobalEventMap::iterator it = recordEvents.begin(); it != recordEvents.end(); ++it)
			it->second->executeRecord(lastPlayersRecord, tmplPlayersRecord);

		savePlayersRecord();
	}
}

void Game::savePlayersRecord()
{
	FILE* file = fopen("playersRecord.txt", "w");
	if(file == NULL)
	{
		std::cout << "> ERROR: Failed to save playersRecord.txt" << std::endl;
		return;
	}

	int32_t tmp = fprintf(file, "%u", lastPlayersRecord);
	if(tmp == EOF)
	{
		std::cout << "> ERROR: Failed to save playersRecord.txt" << std::endl;
		return;
	}

	fclose(file);
}

void Game::loadPlayersRecord()
{
	FILE* file = fopen("playersRecord.txt", "r");
	if(file == NULL)
	{
		std::cout << "> ERROR: Failed to load playersRecord.txt" << std::endl;
		lastPlayersRecord = 0;
		return;
	}

	int32_t tmp = fscanf(file, "%u", &lastPlayersRecord);
	if(tmp == EOF)
	{
		std::cout << "> ERROR: Failed to read playersRecord.txt" << std::endl;
		lastPlayersRecord = 0;
	}
	fclose(file);
}

uint64_t Game::getExperienceStage(uint32_t level)
{
	if(!stagesEnabled)
		return g_config.getNumber(ConfigManager::RATE_EXPERIENCE);

	if(useLastStageLevel && level >= lastStageLevel)
		return stages[lastStageLevel];
	else
		return stages[level];
}

bool Game::loadExperienceStages()
{
	std::string filename = "data/XML/stages.xml";
	xmlDocPtr doc = xmlParseFile(filename.c_str());
	if(doc)
	{
		xmlNodePtr root, p;
		int32_t intVal, low, high, mult;
		root = xmlDocGetRootElement(doc);
		if(xmlStrcmp(root->name,(const xmlChar*)"stages"))
		{
			xmlFreeDoc(doc);
			return false;
		}
		p = root->children;

		while(p)
		{
			if(!xmlStrcmp(p->name, (const xmlChar*)"config"))
			{
				if(readXMLInteger(p, "enabled", intVal))
					stagesEnabled = (intVal != 0);
			}
			else if(!xmlStrcmp(p->name, (const xmlChar*)"stage"))
			{
				if(readXMLInteger(p, "minlevel", intVal))
					low = intVal;
				else
					low = 1;

				if(readXMLInteger(p, "maxlevel", intVal))
					high = intVal;
				else
				{
					high = 0;
					lastStageLevel = low;
					useLastStageLevel = true;
				}

				if(readXMLInteger(p, "multiplier", intVal))
					mult = intVal;
				else
					mult = 1;

				if(useLastStageLevel)
					stages[lastStageLevel] = mult;
				else
				{
					for(int32_t iteratorValue = low; iteratorValue <= high; iteratorValue++)
						stages[iteratorValue] = mult;
				}
			}
			p = p->next;
		}
		xmlFreeDoc(doc);
	}
	return true;
}

bool Game::playerInviteToParty(uint32_t playerId, uint32_t invitedId)
{
	Player* player = getPlayerByID(playerId);
	if(!player || player->isRemoved())
		return false;

	Player* invitedPlayer = getPlayerByID(invitedId);
	if(!invitedPlayer || invitedPlayer->isRemoved() || invitedPlayer->isInviting(player))
		return false;

	if(invitedPlayer->getParty())
	{
		std::ostringstream ss;
		ss << invitedPlayer->getName() << " is already in a party.";
		player->sendTextMessage(MSG_INFO_DESCR, ss.str());
		return false;
	}

	Party* party = player->getParty();
	if(!party)
		party = new Party(player);
	else if(party->getLeader() != player)
		return false;

	return party->invitePlayer(invitedPlayer);
}

bool Game::playerJoinParty(uint32_t playerId, uint32_t leaderId)
{
	Player* player = getPlayerByID(playerId);
	if(!player || player->isRemoved())
		return false;

	Player* leader = getPlayerByID(leaderId);
	if(!leader || leader->isRemoved() || !leader->isInviting(player))
		return false;

	Party* party = leader->getParty();
	if(!party || party->getLeader() != leader)
		return false;

	if(player->getParty())
	{
		player->sendTextMessage(MSG_INFO_DESCR, "You are already in a party.");
		return false;
	}
	return party->joinParty(player);
}

bool Game::playerRevokePartyInvitation(uint32_t playerId, uint32_t invitedId)
{
	Player* player = getPlayerByID(playerId);
	if(!player || player->isRemoved())
		return false;

	Party* party = player->getParty();
	if(!party || party->getLeader() != player)
		return false;

	Player* invitedPlayer = getPlayerByID(invitedId);
	if(!invitedPlayer || invitedPlayer->isRemoved() || !player->isInviting(invitedPlayer))
		return false;

	party->revokeInvitation(invitedPlayer);
	return true;
}

bool Game::playerPassPartyLeadership(uint32_t playerId, uint32_t newLeaderId)
{
	Player* player = getPlayerByID(playerId);
	if(!player || player->isRemoved())
		return false;

	Party* party = player->getParty();
	if(!party || party->getLeader() != player)
		return false;

	Player* newLeader = getPlayerByID(newLeaderId);
	if(!newLeader || newLeader->isRemoved() || !player->isPartner(newLeader))
		return false;

	return party->passPartyLeadership(newLeader);
}

bool Game::playerLeaveParty(uint32_t playerId)
{
	Player* player = getPlayerByID(playerId);
	if(!player || player->isRemoved())
		return false;

	Party* party = player->getParty();
	if(!party || player->hasCondition(CONDITION_INFIGHT))
		return false;

	return party->leaveParty(player);
}

bool Game::playerEnableSharedPartyExperience(uint32_t playerId, bool sharedExpActive)
{
	Player* player = getPlayerByID(playerId);
	if(!player || player->isRemoved())
		return false;

	Party* party = player->getParty();
	if(!party || player->hasCondition(CONDITION_INFIGHT))
		return false;

	return party->setSharedExperience(player, sharedExpActive);
}

void Game::sendGuildMotd(uint32_t playerId, uint32_t guildId)
{
	Player* player = getPlayerByID(playerId);
	if(!player || player->isRemoved())
		return;

	player->sendChannelMessage("Message of the Day", IOGuild::getInstance()->getMotd(guildId), SPEAK_CHANNEL_R1, CHANNEL_GUILD);
}

void Game::kickPlayer(uint32_t playerId, bool displayEffect)
{
	Player* player = getPlayerByID(playerId);
	if(!player || player->isRemoved())
		return;

	player->kickPlayer(displayEffect);
}

bool Game::playerReportBug(uint32_t playerId, std::string bug)
{
	Player* player = getPlayerByID(playerId);
	if(!player || player->isRemoved())
		return false;

	if(player->getAccountType() == ACCOUNT_TYPE_NORMAL)
		return false;

	std::string fileName = "data/reports/" + player->getName() + " report.txt";
	FILE* file = fopen(fileName.c_str(), "a");
	if(file)
	{
		const Position& position = player->getPosition();
		fprintf(file, "------------------------------\nName: %s [Position X: %d Y: %d Z: %d]\nBug Report: %s\n", player->getName().c_str(), position.x, position.y, position.z, bug.c_str());
		fclose(file);
	}

	player->sendTextMessage(MSG_EVENT_DEFAULT, "Your report has been sent to " + g_config.getString(ConfigManager::SERVER_NAME) + ".");
	return true;
}

bool Game::playerDebugAssert(uint32_t playerId, std::string assertLine, std::string date, std::string description, std::string comment)
{
	Player* player = getPlayerByID(playerId);
	if(!player || player->isRemoved())
		return false;

	FILE* file = fopen("client_assertions.txt", "a");
	if(file)
	{
		fprintf(file, "----- %s - %s (%s) -----\n", formatDate(time(NULL)).c_str(), player->getName().c_str(), convertIPToString(player->getIP()).c_str());
		fprintf(file, "%s\n%s\n%s\n%s\n", assertLine.c_str(), date.c_str(), description.c_str(), comment.c_str());
		fclose(file);
	}
	return true;
}

bool Game::playerLeaveMarket(uint32_t playerId)
{
	Player* player = getPlayerByID(playerId);
	if(!player || player->isRemoved())
		return false;

	player->setMarketDepotId(-1);
	return true;
}

bool Game::playerBrowseMarket(uint32_t playerId, uint16_t spriteId)
{
	Player* player = getPlayerByID(playerId);
	if(!player || player->isRemoved())
		return false;

	if(player->getMarketDepotId() == -1)
		return false;

	const ItemType& it = Item::items.getItemIdByClientId(spriteId);
	if(it.id == 0)
		return false;

	if(!it.ware)
		return false;

	const MarketOfferList& buyOffers = IOMarket::getInstance()->getActiveOffers(MARKETACTION_BUY, it.id);
	const MarketOfferList& sellOffers = IOMarket::getInstance()->getActiveOffers(MARKETACTION_SELL, it.id);
	player->sendMarketBrowseItem(it.id, buyOffers, sellOffers);
	player->sendMarketDetail(it.id);
	return true;
}

bool Game::playerBrowseMarketOwnOffers(uint32_t playerId)
{
	Player* player = getPlayerByID(playerId);
	if(!player || player->isRemoved())
		return false;

	if(player->getMarketDepotId() == -1)
		return false;

	const MarketOfferList& buyOffers = IOMarket::getInstance()->getOwnOffers(MARKETACTION_BUY, player->getGUID());
	const MarketOfferList& sellOffers = IOMarket::getInstance()->getOwnOffers(MARKETACTION_SELL, player->getGUID());
	player->sendMarketBrowseOwnOffers(buyOffers, sellOffers);
	return true;
}

bool Game::playerBrowseMarketOwnHistory(uint32_t playerId)
{
	Player* player = getPlayerByID(playerId);
	if(!player || player->isRemoved())
		return false;

	if(player->getMarketDepotId() == -1)
		return false;

	const HistoryMarketOfferList& buyOffers = IOMarket::getInstance()->getOwnHistory(MARKETACTION_BUY, player->getGUID());
	const HistoryMarketOfferList& sellOffers = IOMarket::getInstance()->getOwnHistory(MARKETACTION_SELL, player->getGUID());
	player->sendMarketBrowseOwnHistory(buyOffers, sellOffers);
	return true;
}

bool Game::playerCreateMarketOffer(uint32_t playerId, uint8_t type, uint16_t spriteId, uint16_t amount, uint32_t price, bool anonymous)
{
	if(amount == 0 || amount > 64000)
		return false;

	if(price == 0 || price > 999999999)
		return false;

	if(type != MARKETACTION_BUY && type != MARKETACTION_SELL)
		return false;

	Player* player = getPlayerByID(playerId);
	if(!player || player->isRemoved())
		return false;

	if(player->getMarketDepotId() == -1)
		return false;

	if(g_config.getBoolean(ConfigManager::MARKET_PREMIUM) && !player->isPremium())
	{
		player->sendMarketLeave();
		return false;
	}

	const ItemType& it = Item::items.getItemIdByClientId(spriteId);
	if(it.id == 0)
		return false;

	if(!it.ware)
		return false;

	const int32_t maxOfferCount = g_config.getNumber(ConfigManager::MAX_MARKET_OFFERS_AT_A_TIME_PER_PLAYER);
	if(maxOfferCount > 0)
	{
		const int32_t offerCount = IOMarket::getInstance()->getPlayerOfferCount(player->getGUID());
		if(offerCount == -1 || offerCount >= maxOfferCount)
			return false;
	}

	uint64_t fee = (price / 100.) * amount;
	if(fee < 20)
		fee = 20;
	else if(fee > 1000)
		fee = 1000;

	if(type == MARKETACTION_SELL)
	{
		if(fee > player->bankBalance)
			return false;

		Depot* depot = player->getDepot(player->getMarketDepotId(), false);
		if(!depot)
			return false;

		ItemList itemList;
		uint32_t count = 0;
		std::list<Container*> containerList;
		containerList.push_back(depot->getChest());
		bool enough = false;
		do
		{
			Container* container = containerList.front();
			containerList.pop_front();
			for(ItemList::const_iterator iter = container->getItems(), end = container->getEnd(); iter != end; ++iter)
			{
				Item* item = (*iter);
				Container* c = item->getContainer();
				if(c && !c->empty())
				{
					containerList.push_back(c);
					continue;
				}

				if(item->getID() != it.id)
					continue;

				const ItemType& itemType = Item::items[item->getID()];
				if(!itemType.isRune() && item->getCharges() != itemType.charges)
					continue;

				if(item->getDuration() != itemType.decayTime)
					continue;

				itemList.push_back(item);
				count += Item::countByType(item, -1);
				if(count >= amount)
				{
					enough = true;
					break;
				}
			}

			if(enough)
				break;
		}
		while(!containerList.empty());

		if(!enough)
			return false;

		if(it.stackable)
		{
			uint16_t tmpAmount = amount;
			for(ItemList::const_iterator iter = itemList.begin(), end = itemList.end(); iter != end; ++iter)
			{
				uint16_t removeCount = std::min(tmpAmount, (*iter)->getItemCount());
				tmpAmount -= removeCount;
				internalRemoveItem(*iter, removeCount);
				if(tmpAmount == 0)
					break;
			}
		}
		else
		{
			for(ItemList::const_iterator iter = itemList.begin(), end = itemList.end(); iter != end; ++iter)
				internalRemoveItem(*iter);
		}
		player->bankBalance -= fee;
	}
	else
	{
		uint64_t totalPrice = (uint64_t)price * amount;
		totalPrice += fee;
		if(totalPrice > player->bankBalance)
			return false;

		player->bankBalance -= totalPrice;
	}

	IOMarket::getInstance()->createOffer(player->getGUID(), (MarketAction_t)type, it.id, amount, price, anonymous);

	player->sendMarketEnter(player->getMarketDepotId());
	const MarketOfferList& buyOffers = IOMarket::getInstance()->getActiveOffers(MARKETACTION_BUY, it.id);
	const MarketOfferList& sellOffers = IOMarket::getInstance()->getActiveOffers(MARKETACTION_SELL, it.id);
	player->sendMarketBrowseItem(it.id, buyOffers, sellOffers);
	return true;
}

bool Game::playerCancelMarketOffer(uint32_t playerId, uint32_t timestamp, uint16_t counter)
{
	Player* player = getPlayerByID(playerId);
	if(!player || player->isRemoved())
		return false;

	if(player->getMarketDepotId() == -1)
		return false;

	uint32_t offerId = IOMarket::getInstance()->getOfferIdByCounter(timestamp, counter);
	if(offerId == 0)
		return false;

	MarketOfferEx offer = IOMarket::getInstance()->getOfferById(offerId);
	if(offer.playerId != player->getGUID())
		return false;

	if(offer.type == MARKETACTION_BUY)
	{
		player->bankBalance += (uint64_t)offer.price * offer.amount;
		player->sendMarketEnter(player->getMarketDepotId());
	}
	else
	{
		const ItemType& it = Item::items[offer.itemId];
		if(it.id == 0)
			return false;

		Depot* depot = player->getDepot(player->getMarketDepotId(), true);
		if(it.stackable)
		{
			uint16_t tmpAmount = offer.amount;
			while(tmpAmount > 0)
			{
				int32_t stackCount = std::min((int32_t)100, (int32_t)tmpAmount);
				Item* item = Item::CreateItem(it.id, stackCount);
				if(internalAddItem(depot->getInbox(), item, INDEX_WHEREEVER, FLAG_NOLIMIT) != RET_NOERROR)
				{
					delete item;
					break;
				}

				tmpAmount -= stackCount;
			}
		}
		else
		{
			int32_t subType = -1;
			if(it.charges != 0)
				subType = it.charges;

			for(uint16_t i = 0; i < offer.amount; ++i)
			{
				Item* item = Item::CreateItem(it.id, subType);
				if(internalAddItem(depot->getInbox(), item, INDEX_WHEREEVER, FLAG_NOLIMIT) != RET_NOERROR)
				{
					delete item;
					break;
				}
			}
		}
	}

	IOMarket::getInstance()->moveOfferToHistory(offerId, OFFERSTATE_CANCELLED);
	offer.amount = 0;
	offer.timestamp += g_config.getNumber(ConfigManager::MARKET_OFFER_DURATION);
	player->sendMarketCancelOffer(offer);
	return true;
}

bool Game::playerAcceptMarketOffer(uint32_t playerId, uint32_t timestamp, uint16_t counter, uint16_t amount)
{
	if(amount == 0 || amount > 64000)
		return false;

	Player* player = getPlayerByID(playerId);
	if(!player || player->isRemoved())
		return false;

	if(player->getMarketDepotId() == -1)
		return false;

	uint32_t offerId = IOMarket::getInstance()->getOfferIdByCounter(timestamp, counter);
	if(offerId == 0)
		return false;

	MarketOfferEx offer = IOMarket::getInstance()->getOfferById(offerId);
	if(amount > offer.amount)
		return false;

	const ItemType& it = Item::items[offer.itemId];
	if(it.id == 0)
		return false;

	uint64_t totalPrice = (uint64_t)offer.price * amount;
	if(offer.type == MARKETACTION_BUY)
	{
		Depot* depot = player->getDepot(player->getMarketDepotId(), false);
		if(!depot)
			return false;

		ItemList itemList;
		uint32_t count = 0;
		std::list<Container*> containerList;
		containerList.push_back(depot->getChest());
		bool enough = false;
		do
		{
			Container* container = containerList.front();
			containerList.pop_front();
			for(ItemList::const_iterator iter = container->getItems(), end = container->getEnd(); iter != end; ++iter)
			{
				Item* item = (*iter);
				Container* c = item->getContainer();
				if(c && !c->empty())
				{
					containerList.push_back(c);
					continue;
				}

				if(item->getID() != it.id)
					continue;

				const ItemType& itemType = Item::items[item->getID()];
				if(!itemType.isRune() && item->getCharges() != itemType.charges)
					continue;

				if(item->getDuration() != itemType.decayTime)
					continue;

				itemList.push_back(item);
				count += Item::countByType(item, -1);
				if(count >= amount)
				{
					enough = true;
					break;
				}
			}

			if(enough)
				break;
		}
		while(!containerList.empty());

		if(!enough)
			return false;

		if(it.stackable)
		{
			uint16_t tmpAmount = amount;
			for(ItemList::const_iterator iter = itemList.begin(), end = itemList.end(); iter != end; ++iter)
			{
				uint16_t removeCount = std::min(tmpAmount, (*iter)->getItemCount());
				tmpAmount -= removeCount;
				internalRemoveItem(*iter, removeCount);
				if(tmpAmount == 0)
					break;
			}
		}
		else
		{
			for(ItemList::const_iterator iter = itemList.begin(), end = itemList.end(); iter != end; ++iter)
				internalRemoveItem(*iter);
		}

		player->bankBalance += totalPrice;

		Player* buyerPlayer = getPlayerByGUID(offer.playerId);
		if(!buyerPlayer)
		{
			std::string buyerName;
			if(!IOLoginData::getInstance()->getNameByGuid(offer.playerId, buyerName))
				return false;

			buyerPlayer = new Player(buyerName, NULL);
			if(!IOLoginData::getInstance()->loadPlayer(buyerPlayer, buyerName))
			{
				delete buyerPlayer;
				return false;
			}
		}
		buyerPlayer->setDepotChange(true);

		Depot* buyerDepot = buyerPlayer->getDepot(buyerPlayer->getTown(), true);
		if(it.stackable)
		{
			uint16_t tmpAmount = amount;
			while(tmpAmount > 0)
			{
				uint16_t stackCount = std::min((uint16_t)100, tmpAmount);
				Item* item = Item::CreateItem(it.id, stackCount);
				if(internalAddItem(buyerDepot->getInbox(), item, INDEX_WHEREEVER, FLAG_NOLIMIT) != RET_NOERROR)
				{
					delete item;
					break;
				}

				tmpAmount -= stackCount;
			}
		}
		else
		{
			int32_t subType = -1;
			if(it.charges != 0)
				subType = it.charges;

			for(uint16_t i = 0; i < amount; ++i)
			{
				Item* item = Item::CreateItem(it.id, subType);
				if(internalAddItem(buyerDepot->getInbox(), item, INDEX_WHEREEVER, FLAG_NOLIMIT) != RET_NOERROR)
				{
					delete item;
					break;
				}
			}
		}

		if(buyerPlayer->isOffline())
		{
			IOLoginData::getInstance()->savePlayer(buyerPlayer, true);
			delete buyerPlayer;
		}
	}
	else
	{
		if(totalPrice > player->bankBalance)
			return false;

		player->bankBalance -= totalPrice;

		Depot* depot = player->getDepot(player->getMarketDepotId(), true);
		if(it.stackable)
		{
			uint16_t tmpAmount = amount;
			while(tmpAmount > 0)
			{
				uint16_t stackCount = std::min((uint16_t)100, tmpAmount);
				Item* item = Item::CreateItem(it.id, stackCount);
				if(internalAddItem(depot->getInbox(), item, INDEX_WHEREEVER, FLAG_NOLIMIT) != RET_NOERROR)
				{
					delete item;
					break;
				}

				tmpAmount -= stackCount;
			}
		}
		else
		{
			int32_t subType = -1;
			if(it.charges != 0)
				subType = it.charges;

			for(uint16_t i = 0; i < amount; ++i)
			{
				Item* item = Item::CreateItem(it.id, subType);
				if(internalAddItem(depot->getInbox(), item, INDEX_WHEREEVER, FLAG_NOLIMIT) != RET_NOERROR)
				{
					delete item;
					break;
				}
			}
		}

		Player* sellerPlayer = getPlayerByGUID(offer.playerId);
		if(sellerPlayer)
			sellerPlayer->bankBalance += totalPrice;
		else
			IOLoginData::getInstance()->increaseBankBalance(offer.playerId, totalPrice);
	}

	const int32_t marketOfferDuration = g_config.getNumber(ConfigManager::MARKET_OFFER_DURATION);
	IOMarket::getInstance()->appendHistory(player->getGUID(), (offer.type == MARKETACTION_BUY ? MARKETACTION_SELL : MARKETACTION_BUY), offer.itemId, amount, offer.price, offer.timestamp + marketOfferDuration, OFFERSTATE_ACCEPTEDEX);
	IOMarket::getInstance()->appendHistory(offer.playerId, offer.type, offer.itemId, amount, offer.price, offer.timestamp + marketOfferDuration, OFFERSTATE_ACCEPTED);

	offer.amount -= amount;
	if(offer.amount == 0)
		IOMarket::getInstance()->deleteOffer(offerId);
	else
		IOMarket::getInstance()->acceptOffer(offerId, amount);

	player->sendMarketEnter(player->getMarketDepotId());
	offer.timestamp += marketOfferDuration;
	player->sendMarketAcceptOffer(offer);
	return true;
}

void Game::checkExpiredMarketOffers()
{
	IOMarket::getInstance()->clearOldHistory();

	const ExpiredMarketOfferList& expiredBuyOffers = IOMarket::getInstance()->getExpiredOffers(MARKETACTION_BUY);
	for(ExpiredMarketOfferList::const_iterator it = expiredBuyOffers.begin(), end = expiredBuyOffers.end(); it != end; ++it)
	{
		ExpiredMarketOffer offer = *it;

		Player* player = getPlayerByGUID(offer.playerId);
		uint64_t totalPrice = (uint64_t)offer.price * offer.amount;
		if(player)
			player->bankBalance += totalPrice;
		else
			IOLoginData::getInstance()->increaseBankBalance(offer.playerId, totalPrice);

		IOMarket::getInstance()->moveOfferToHistory(offer.id, OFFERSTATE_EXPIRED);
	}

	const ExpiredMarketOfferList& expiredSellOffers = IOMarket::getInstance()->getExpiredOffers(MARKETACTION_SELL);
	for(ExpiredMarketOfferList::const_iterator it = expiredSellOffers.begin(), end = expiredSellOffers.end(); it != end; ++it)
	{
		ExpiredMarketOffer offer = *it;

		Player* player = getPlayerByGUID(offer.playerId);
		if(!player)
		{
			std::string name;
			if(!IOLoginData::getInstance()->getNameByGuid(offer.playerId, name))
				continue;

			player = new Player(name, NULL);
			if(!IOLoginData::getInstance()->loadPlayer(player, name))
			{
				delete player;
				continue;
			}
		}
		player->setDepotChange(true);

		const ItemType& itemType = Item::items[offer.itemId];
		if(itemType.id == 0)
			continue;

		Depot* depot = player->getDepot(player->getTown(), true);
		if(itemType.stackable)
		{
			uint16_t tmpAmount = offer.amount;
			while(tmpAmount > 0)
			{
				uint16_t stackCount = std::min((uint16_t)100, tmpAmount);
				Item* item = Item::CreateItem(itemType.id, stackCount);
				if(internalAddItem(depot->getInbox(), item, INDEX_WHEREEVER, FLAG_NOLIMIT) != RET_NOERROR)
				{
					delete item;
					break;
				}

				tmpAmount -= stackCount;
			}
		}
		else
		{
			int32_t subType = -1;
			if(itemType.charges != 0)
				subType = itemType.charges;

			for(uint16_t i = 0; i < offer.amount; ++i)
			{
				Item* item = Item::CreateItem(itemType.id, subType);
				if(internalAddItem(depot->getInbox(), item, INDEX_WHEREEVER, FLAG_NOLIMIT) != RET_NOERROR)
				{
					delete item;
					break;
				}
			}
		}

		if(player->isOffline())
		{
			IOLoginData::getInstance()->savePlayer(player, true);
			delete player;
		}

		IOMarket::getInstance()->moveOfferToHistory(offer.id, OFFERSTATE_EXPIRED);
	}

	int32_t checkExpiredMarketOffersEachMinutes = g_config.getNumber(ConfigManager::CHECK_EXPIRED_MARKET_OFFERS_EACH_MINUTES);
	if(checkExpiredMarketOffersEachMinutes <= 0)
		return;

	g_scheduler.addEvent(createSchedulerTask(checkExpiredMarketOffersEachMinutes * 60 * 1000, boost::bind(&Game::checkExpiredMarketOffers, this)));
}

void Game::forceAddCondition(uint32_t creatureId, Condition* condition)
{
	Creature* creature = getCreatureByID(creatureId);
	if(!creature || creature->isRemoved())
		return;

	creature->addCondition(condition, true);
}

void Game::forceRemoveCondition(uint32_t creatureId, ConditionType_t type)
{
	Creature* creature = getCreatureByID(creatureId);
	if(!creature || creature->isRemoved())
		return;

	creature->removeCondition(type, true);
}

bool Game::violationWindow(Player* player, std::string targetPlayerName, int32_t reason, int32_t action, std::string banComment, bool IPBanishment)
{
	if((0 == (violationActions[player->getAccountType()] & (1 << action)))
		|| reason > violationReasons[player->getAccountType()]
		|| (IPBanishment && (violationActions[player->getAccountType()] & Action_IpBan) != Action_IpBan))
	{
		player->sendCancel("You do not have authorization for this action.");
		return false;
	}

	//If this will be configurable, the field in the database has to be edited
	//since its a VARCHAR(60).
	if(banComment.size() > 60)
	{
		player->sendCancel("The comment may not exceed the limit of 60 characters.");
		return false;
	}

	bool playerExists = false;
	if(IOLoginData::getInstance()->playerExists(targetPlayerName))
		playerExists = true;

	toLowerCaseString(targetPlayerName);
	if(!playerExists || (g_config.getBoolean(ConfigManager::ACCOUNT_MANAGER) && targetPlayerName == "account manager"))
	{
		player->sendCancel("A player with this name does not exist.");
		return false;
	}

	AccountType_t targetAccountType = ACCOUNT_TYPE_NORMAL;
	Account account;
	uint32_t guid;
	Player* targetPlayer = getPlayerByName(targetPlayerName);
	if(targetPlayer)
	{
		targetAccountType = targetPlayer->getAccountType();
		guid = targetPlayer->getGUID();
		account = IOLoginData::getInstance()->loadAccount(targetPlayer->getAccount());
		targetPlayerName = targetPlayer->getName();
	}
	else
	{
		targetAccountType = IOLoginData::getInstance()->getAccountType(targetPlayerName);
		IOLoginData::getInstance()->getGuidByName(guid, targetPlayerName);
		account = IOLoginData::getInstance()->loadAccount(IOLoginData::getInstance()->getAccountNumberByName(targetPlayerName));
	}

	if(targetAccountType >= player->getAccountType())
	{
		player->sendCancel("You do not have authorization for this action.");
		return false;
	}

	bool isNotation = false;
	switch(action)
	{
		case 0:
		{
			IOBan::getInstance()->addAccountNotation(account.id, time(NULL), reason, action, banComment, player->getGUID());
			if(IOBan::getInstance()->getNotationsCount(account.id) > 2)
			{
				account.warnings++;
				if(account.warnings > 3)
				{
					action = 7;
					IOBan::getInstance()->addAccountDeletion(account.id, time(NULL), reason, action, banComment, player->getGUID());
				}
				else if(account.warnings == 3)
					IOBan::getInstance()->addAccountBan(account.id, (time(NULL) + (g_config.getNumber(ConfigManager::FINAL_BAN_DAYS) * 86400)), reason, action, banComment, player->getGUID());
				else
					IOBan::getInstance()->addAccountBan(account.id, (time(NULL) + (g_config.getNumber(ConfigManager::BAN_DAYS) * 86400)), reason, action, "4 notations received, auto banishment.", player->getGUID());
			}
			else
				isNotation = true;

			break;
		}

		case 1:
		{
			IOBan::getInstance()->addPlayerNamelock(guid, time(NULL), reason, action, banComment, player->getGUID());
			break;
		}

		case 3:
		{
			account.warnings++;
			if(account.warnings > 3)
			{
				action = 7;
				IOBan::getInstance()->addAccountDeletion(account.id, time(NULL), reason, action, banComment, player->getGUID());
			}
			else
			{
				if(account.warnings == 3)
					IOBan::getInstance()->addAccountBan(account.id, (time(NULL) + (g_config.getNumber(ConfigManager::FINAL_BAN_DAYS) * 86400)), reason, action, banComment, player->getGUID());
				else
					IOBan::getInstance()->addAccountBan(account.id, (time(NULL) + (g_config.getNumber(ConfigManager::BAN_DAYS) * 86400)), reason, action, banComment, player->getGUID());

				IOBan::getInstance()->addPlayerNamelock(guid, time(NULL), reason, action, banComment, player->getGUID());
			}
			break;
		}

		case 4:
		{
			if(account.warnings < 3)
			{
				account.warnings = 3;
				IOBan::getInstance()->addAccountBan(account.id, (time(NULL) + (g_config.getNumber(ConfigManager::FINAL_BAN_DAYS) * 86400)), reason, action, banComment, player->getGUID());
			}
			else
			{
				action = 7;
				account.warnings++;
				IOBan::getInstance()->addAccountDeletion(account.id, time(NULL), reason, action, banComment, player->getGUID());
			}
			break;
		}

		case 5:
		{
			if(account.warnings < 3)
			{
				account.warnings = 3;
				IOBan::getInstance()->addAccountBan(account.id, (time(NULL) + (g_config.getNumber(ConfigManager::FINAL_BAN_DAYS) * 86400)), reason, action, banComment, player->getGUID());
				IOBan::getInstance()->addPlayerNamelock(guid, time(NULL), reason, action, banComment, player->getGUID());
			}
			else
			{
				action = 7;
				account.warnings++;
				IOBan::getInstance()->addAccountDeletion(account.id, time(NULL), reason, action, banComment, player->getGUID());
			}
			break;
		}

		default:
		{
			account.warnings++;
			if(account.warnings > 3)
			{
				action = 7;
				IOBan::getInstance()->addAccountDeletion(account.id, time(NULL), reason, action, banComment, player->getGUID());
			}
			else
			{
				if(account.warnings == 3)
					IOBan::getInstance()->addAccountBan(account.id, (time(NULL) + (g_config.getNumber(ConfigManager::FINAL_BAN_DAYS) * 86400)), reason, action, banComment, player->getGUID());
				else
					IOBan::getInstance()->addAccountBan(account.id, (time(NULL) + (g_config.getNumber(ConfigManager::BAN_DAYS) * 86400)), reason, action, banComment, player->getGUID());
			}
			break;
		}
	}

	if(g_config.getBoolean(ConfigManager::BROADCAST_BANISHMENTS))
	{
		std::ostringstream ss;
		if(isNotation)
			ss << targetPlayerName << " has received a notation by " << player->getName() << " (" << (3 - IOBan::getInstance()->getNotationsCount(account.id)) << " more to ban).";
		else
			ss << player->getName() << " has taken the action \"" << getAction(action, IPBanishment) << "\" against: " << targetPlayerName << " (Warnings: " << account.warnings << "), with reason: \"" << getReason(reason) << "\", and comment: \"" << banComment << "\".";

		broadcastMessage(ss.str(), MSG_STATUS_WARNING);
	}
	else
	{
		std::ostringstream ss;
		if(isNotation)
			ss << "You have taken the action notation against " << targetPlayerName << " (" << (3 - IOBan::getInstance()->getNotationsCount(account.id)) << " more to ban).";
		else
			ss << "You have taken the action \"" << getAction(action, IPBanishment) << "\" against: " << targetPlayerName << " (Warnings: " << account.warnings << "), with reason: \"" << getReason(reason) << "\", and comment: \"" << banComment << "\".";

		player->sendTextMessage(MSG_STATUS_CONSOLE_RED, ss.str());
	}

	if(targetPlayer)
	{
		if(IPBanishment)
		{
			uint32_t ip = targetPlayer->lastIP;
			if(ip > 0)
				IOBan::getInstance()->addIpBan(ip, 0xFFFFFFFF, (time(NULL) + 86400));
		}

		if(!isNotation)
		{
			addMagicEffect(targetPlayer->getPosition(), NM_ME_MAGIC_POISON);

			uint32_t playerId = targetPlayer->getID();
			g_scheduler.addEvent(createSchedulerTask(1000, boost::bind(&Game::kickPlayer, this, playerId, false)));
		}
	}
	else if(IPBanishment)
	{
		uint32_t lastip = IOLoginData::getInstance()->getLastIPByName(targetPlayerName);
		if(lastip != 0)
			IOBan::getInstance()->addIpBan(lastip, 0xFFFFFFFF, (time(NULL) + 86400));
	}

	if(!isNotation)
		IOBan::getInstance()->removeAccountNotations(account.id);

	IOLoginData::getInstance()->saveAccount(account);
	return true;
}

bool Game::loadStatuslist()
{
	xmlDocPtr doc = xmlParseFile("http://forgottenserver.otland.net/statuslist.xml");
	if(!doc)
		return false;

	xmlNodePtr p, root = xmlDocGetRootElement(doc);
	if(!xmlStrcmp(root->name, (const xmlChar*)"statuslist"))
	{
		p = root->children;
		while(p)
		{
			if(!xmlStrcmp(p->name, (const xmlChar*)"blacklist"))
			{
				std::string ip;
				if(readXMLString(p, "ip", ip))
					blacklist.push_back(ip);
			}
			else if(!xmlStrcmp(p->name, (const xmlChar*)"whitelist"))
			{
				std::string ip;
				if(readXMLString(p, "ip", ip))
					whitelist.push_back(ip);
			}

			p = p->next;
		}
	}

	xmlFreeDoc(doc);
	return true;
}
