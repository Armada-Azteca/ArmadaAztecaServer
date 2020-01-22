//////////////////////////////////////////////////////////////////////
// OpenTibia - an opensource roleplaying game
//////////////////////////////////////////////////////////////////////
// Implementation of game protocol
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

#include "protocolgame.h"

#include "networkmessage.h"
#include "outputmessage.h"

#include "items.h"

#include "tile.h"
#include "player.h"
#include "chat.h"

#include "configmanager.h"
#include "actions.h"
#include "game.h"
#include "iologindata.h"
#include "iomarket.h"
#include "house.h"
#include "waitlist.h"
#include "quests.h"
#include "mounts.h"
#include "ban.h"
#include "connection.h"
#include "creatureevent.h"

#include <string>
#include <iostream>
#include <sstream>
#include <time.h>
#include <list>
#include <iomanip>

#include <boost/function.hpp>

extern Game g_game;
extern ConfigManager g_config;
extern Actions actions;
extern Ban g_bans;
extern CreatureEvents* g_creatureEvents;
Chat g_chat;

#ifdef __ENABLE_SERVER_DIAGNOSTIC__
uint32_t ProtocolGame::protocolGameCount = 0;
#endif

// Helping templates to add dispatcher tasks
template<class FunctionType>
void ProtocolGame::addGameTaskInternal(bool droppable, uint32_t delay, const FunctionType& func)
{
	if(droppable)
		g_dispatcher.addTask(createTask(delay, func));
	else
		g_dispatcher.addTask(createTask(func));
}

ProtocolGame::ProtocolGame(Connection_ptr connection) :
	Protocol(connection)
{
	player = NULL;
	m_debugAssertSent = false;
	m_acceptPackets = false;
	eventConnect = 0;
#ifdef __ENABLE_SERVER_DIAGNOSTIC__
	protocolGameCount++;
#endif
}

ProtocolGame::~ProtocolGame()
{
	player = NULL;
#ifdef __ENABLE_SERVER_DIAGNOSTIC__
	protocolGameCount--;
#endif
}

void ProtocolGame::setPlayer(Player* p)
{
	player = p;
}

void ProtocolGame::releaseProtocol()
{
	//dispatcher thread
	if(player && player->client == this)
		player->client = NULL;

	Protocol::releaseProtocol();
}

void ProtocolGame::deleteProtocolTask()
{
	//dispatcher thread
	if(player)
	{
		#ifdef __DEBUG_NET_DETAIL__
		std::cout << "Deleting ProtocolGame - Protocol:" << this << ", Player: " << player << std::endl;
		#endif

		g_game.FreeThing(player);
		player = NULL;
	}
	Protocol::deleteProtocolTask();
}

bool ProtocolGame::login(const std::string& name, uint32_t accnumber, const std::string& password, uint16_t operatingSystem, uint8_t gamemasterLogin, std::list<uint8_t> openChannels)
{
	//dispatcher thread
	Player* _player = g_game.getPlayerByName(name);
	bool isAccountManager = g_config.getBoolean(ConfigManager::ACCOUNT_MANAGER) && name == "Account Manager";
	if(!_player || g_config.getBoolean(ConfigManager::ALLOW_CLONES) || isAccountManager)
	{
		player = new Player(name, this);

		player->useThing2();
		player->setID();

		if(isAccountManager)
			player->setAccountManager();

		if(IOBan::getInstance()->isPlayerNamelocked(name) && accnumber > 1)
		{
			if(g_config.getBoolean(ConfigManager::ACCOUNT_MANAGER))
			{
				player->name = "Account Manager";
				AccountManager* accountManager = player->getAccountManager();
				if(!accountManager)
				{
					player->setAccountManager();
					accountManager = player->getAccountManager();
				}

				accountManager->namelockedPlayerName = name;
			}
			else
			{
				disconnectClient(0x14, "Your character has been namelocked.");
				return false;
			}
		}

		if(isAccountManager && accnumber != 1)
		{
			AccountManager* accountManager = player->getAccountManager();
			if(!accountManager)
			{
				player->setAccountManager();
				accountManager = player->getAccountManager();
			}

			accountManager->managingAccount = true;
			accountManager->realAccount = accnumber;
		}

		if(!IOLoginData::getInstance()->loadPlayer(player, name, true))
		{
#ifdef __DEBUG__
			std::cout << "ProtocolGame::login - loadPlayer failed - " << name << std::endl;
#endif
			disconnectClient(0x14, "Your character could not be loaded.");
			return false;
		}

		if(gamemasterLogin == 1 && player->getAccountType() < ACCOUNT_TYPE_GAMEMASTER && !isAccountManager)
		{
			disconnectClient(0x14, "You are not a gamemaster!");
			return false;
		}

		if(g_game.getGameState() == GAME_STATE_CLOSING && !player->hasFlag(PlayerFlag_CanAlwaysLogin))
		{
			disconnectClient(0x14, "The game is just going down.\nPlease try again later.");
			return false;
		}

		if(g_game.getGameState() == GAME_STATE_CLOSED && !player->hasFlag(PlayerFlag_CanAlwaysLogin))
		{
			disconnectClient(0x14, "Server is currently closed. Please try again later.");
			return false;
		}

		if(g_config.getBoolean(ConfigManager::ONE_PLAYER_ON_ACCOUNT) && player->getAccountType() < ACCOUNT_TYPE_GAMEMASTER && !isAccountManager && g_game.getPlayerByAccount(player->getAccount()))
		{
			disconnectClient(0x14, "You may only login with one character\nof your account at the same time.");
			return false;
		}

		if(!player->hasFlag(PlayerFlag_CannotBeBanned))
		{
			uint32_t bannedBy = 0, banTime = 0;
			int32_t reason = 0, action = 0;
			std::string comment = "";
			bool deletion = false;
			if(IOBan::getInstance()->getBanInformation(accnumber, bannedBy, banTime, reason, action, comment, deletion))
			{
				uint64_t timeNow = time(NULL);
				if((deletion && banTime != 0) || banTime > timeNow)
				{
					std::string bannedByName;
					if(bannedBy == 0)
						bannedByName = (deletion ? "Automatic deletion" : "Automatic banishment");
					else
						IOLoginData::getInstance()->getNameByGuid(bannedBy, bannedByName);

					std::ostringstream ss;
					ss << "Your account has been " << (deletion ? "deleted" : "banished") << " by:\n" << bannedByName << ", for the following reason:\n" << getReason(reason) << ".\nThe action taken was:\n" << getAction(action, false) << ".\nThe comment given was:\n" << comment << "\nYour " << (deletion ? "account was deleted on" : "banishment will be lifted at") << ":\n" << formatDateShort(banTime) << ".";
					disconnectClient(0x14, ss.str().c_str());
					return false;
				}
			}
		}

		if(!WaitingList::getInstance()->clientLogin(player))
		{
			int32_t currentSlot = WaitingList::getInstance()->getClientSlot(player);
			int32_t retryTime = WaitingList::getTime(currentSlot);
			std::ostringstream ss;

			ss << "Too many players online.\n" << "You are at place "
				<< currentSlot << " on the waiting list.";

			OutputMessage_ptr output = OutputMessagePool::getInstance()->getOutputMessage(this, false);
			if(output)
			{
				TRACK_MESSAGE(output);
				output->AddByte(0x16);
				output->AddString(ss.str());
				output->AddByte(retryTime);
				OutputMessagePool::getInstance()->send(output);
			}
			getConnection()->closeConnection();
			return false;
		}

		if(!IOLoginData::getInstance()->loadPlayer(player, name))
		{
			disconnectClient(0x14, "Your character could not be loaded.");
			return false;
		}

		if(!g_game.placeCreature(player, player->getLoginPosition()))
		{
			if(!g_game.placeCreature(player, player->getTemplePosition(), false, true))
			{
				disconnectClient(0x14, "Temple position is wrong. Contact the administrator.");
				return false;
			}
		}

		player->lastIP = player->getIP();
		player->lastLoginSaved = std::max(time(NULL), player->lastLoginSaved + 1);
		player->setOperatingSystem((OperatingSystem_t)operatingSystem);
		m_acceptPackets = true;

		for(std::list<uint8_t>::const_iterator it = openChannels.begin(), end = openChannels.end(); it != end; ++it)
			addGameTask(&Game::playerOpenChannel, player->getID(), *it);

		return true;
	}
	else
	{
		if(eventConnect != 0 || !g_config.getBoolean(ConfigManager::REPLACE_KICK_ON_LOGIN))
		{
			//Already trying to connect
			disconnectClient(0x14, "You are already logged in.");
			return false;
		}

		g_chat.removeUserFromAllChannels(_player);
		if(_player->client)
		{
			_player->disconnect();
			_player->isConnecting = true;
			addRef();
			eventConnect = g_scheduler.addEvent(
				createSchedulerTask(1000, boost::bind(&ProtocolGame::connect, this, _player->getID(), openChannels)));

			return true;
		}

		addRef();
		return connect(_player->getID(), openChannels);
	}
	return false;
}

bool ProtocolGame::connect(uint32_t playerId, std::list<uint8_t> openChannels)
{
	unRef();
	eventConnect = 0;
	Player* _player = g_game.getPlayerByID(playerId);
	if(!_player || _player->isRemoved() || _player->client)
	{
		disconnectClient(0x14, "You are already logged in.");
		return false;
	}

	player = _player;
	player->useThing2();
	player->isConnecting = false;
	player->client = this;
	player->client->sendAddCreature(player, player->getPosition(),
		player->getTile()->__getIndexOfThing(player), false);
	player->lastIP = player->getIP();
	player->lastLoginSaved = std::max(time(NULL), player->lastLoginSaved + 1);
	m_acceptPackets = true;
	for(std::list<uint8_t>::const_iterator it = openChannels.begin(), end = openChannels.end(); it != end; ++it)
		addGameTask(&Game::playerOpenChannel, player->getID(), *it);

	return true;
}

bool ProtocolGame::logout(bool displayEffect, bool forced)
{
	//dispatcher thread
	if(!player)
		return false;

	if(!player->isRemoved())
	{
		if(forced)
			g_creatureEvents->playerLogout(player);
		else
		{
			bool flag = (player->getAccountType() == ACCOUNT_TYPE_GOD);
			if(player->getTile()->hasFlag(TILESTATE_NOLOGOUT) && !flag)
			{
				player->sendCancelMessage(RET_YOUCANNOTLOGOUTHERE);
				return false;
			}

			if(!player->getTile()->hasFlag(TILESTATE_PROTECTIONZONE) && player->hasCondition(CONDITION_INFIGHT) && !flag)
			{
				player->sendCancelMessage(RET_YOUMAYNOTLOGOUTDURINGAFIGHT);
				return false;
			}

			//scripting event - onLogout
			if(!g_creatureEvents->playerLogout(player) && !flag)
			{
				//Let the script handle the error message
				return false;
			}
		}
	}

	if(player->isRemoved() || player->getHealth() <= 0)
		displayEffect = false;

	if(displayEffect)
		g_game.addMagicEffect(player->getPosition(), NM_ME_POFF);

	if(Connection_ptr connection = getConnection())
		connection->closeConnection();

	return g_game.removeCreature(player);
}

bool ProtocolGame::parseFirstPacket(NetworkMessage& msg)
{
	if(g_game.getGameState() == GAME_STATE_SHUTDOWN)
	{
		getConnection()->closeConnection();
		return false;
	}

	uint16_t clientos = msg.GetU16();
	uint16_t version = msg.GetU16();

	if(!RSA_decrypt(msg))
	{
		getConnection()->closeConnection();
		return false;
	}

	uint32_t key[4];
	key[0] = msg.GetU32();
	key[1] = msg.GetU32();
	key[2] = msg.GetU32();
	key[3] = msg.GetU32();
	enableXTEAEncryption();
	setXTEAKey(key);

	uint8_t isSetGM = msg.GetByte();
	std::string accountName = msg.GetString();
	std::string name = msg.GetString();
	std::string password = msg.GetString();

	msg.SkipBytes(4); // challenge timestamp
	msg.SkipBytes(1); // challenge random

	std::list<uint8_t> openChannels;
	for(uint8_t i = 0, channels = msg.GetByte(); i < channels; ++i)
		openChannels.push_back(msg.GetByte());

	if(version < CLIENT_VERSION_MIN || version > CLIENT_VERSION_MAX)
	{
		disconnectClient(0x14, "Only clients with protocol " CLIENT_VERSION_STR " allowed!");
		return false;
	}

	if(accountName.empty())
	{
		if(g_config.getBoolean(ConfigManager::ACCOUNT_MANAGER))
		{
			accountName = "1";
			password = "1";
		}
		else
		{
			disconnectClient(0x14, "You must enter your account name.");
			return false;
		}
	}

	if(g_game.getGameState() == GAME_STATE_STARTUP || g_game.getServerSaveMessage(0))
	{
		disconnectClient(0x14, "Gameworld is starting up. Please wait.");
		return false;
	}

	if(g_game.getGameState() == GAME_STATE_MAINTAIN)
	{
		disconnectClient(0x14, "Gameworld is under maintenance. Please re-connect in a while.");
		return false;
	}

	if(g_bans.isIpDisabled(getIP()))
	{
		disconnectClient(0x14, "Too many connections attempts from this IP. Try again later.");
		return false;
	}

	if(IOBan::getInstance()->isIpBanished(getIP()))
	{
		disconnectClient(0x14, "Your IP is banished!");
		return false;
	}

	if(!IOLoginData::getInstance()->playerExists(name))
	{
		disconnectClient(0x14, "Player not found.");
		return false;
	}

	std::string acc_pass;
	uint32_t accnumber;
	bool gotPassword;
	if(g_config.getBoolean(ConfigManager::ACCOUNT_MANAGER) && name == "Account Manager")
		gotPassword = IOLoginData::getInstance()->getPasswordEx(accountName, acc_pass, accnumber);
	else
		gotPassword = IOLoginData::getInstance()->getPassword(accountName, name, acc_pass, accnumber);

	if(!gotPassword || !passwordTest(password, acc_pass))
	{
		g_bans.addLoginAttempt(getIP(), false);
		disconnectClient(0x14, "Account name or password is not correct.");
		getConnection()->closeConnection();
		return false;
	}

	g_bans.addLoginAttempt(getIP(), true);

	g_dispatcher.addTask(
		createTask(boost::bind(&ProtocolGame::login, this, name, accnumber, password, clientos, isSetGM, openChannels)));

	return true;
}

void ProtocolGame::onRecvFirstMessage(NetworkMessage& msg)
{
	parseFirstPacket(msg);
}

void ProtocolGame::onConnect()
{
	OutputMessage_ptr output = OutputMessagePool::getInstance()->getOutputMessage(this, false);
	if(output)
	{
		TRACK_MESSAGE(output);

		//Adler checksum
		output->AddByte(0xEF);
		output->AddByte(0x00);
		output->AddByte(0x82);
		output->AddByte(0x02);

		// Unknown
		output->AddByte(0x06);
		output->AddByte(0x00);
		output->AddByte(0x1F);

		output->AddByte(0x01);
		output->AddByte(0x41);

		output->AddU16(0x00);

		output->AddByte(0x87);

		OutputMessagePool::getInstance()->send(output);
	}
}

void ProtocolGame::disconnectClient(uint8_t error, const char* message)
{
	OutputMessage_ptr output = OutputMessagePool::getInstance()->getOutputMessage(this, false);
	if(output)
	{
		TRACK_MESSAGE(output);
		output->AddByte(error);
		output->AddString(message);
		OutputMessagePool::getInstance()->send(output);
	}
	disconnect();
}

void ProtocolGame::disconnect()
{
	if(getConnection())
		getConnection()->closeConnection();
}

void ProtocolGame::parsePacket(NetworkMessage &msg)
{
	if(!player || !m_acceptPackets || g_game.getGameState() == GAME_STATE_SHUTDOWN || msg.getMessageLength() <= 0)
		return;

	uint8_t recvbyte = msg.GetByte();
	//a dead player can not performs actions
	if((player->isRemoved() || player->getHealth() <= 0) && recvbyte != 0x14)
		return;

	if(player->isAccountManager())
	{
		switch(recvbyte)
		{
			case 0x14:
				parseLogout(msg);
				break;

			case 0x1D:
				parseReceivePingBack(msg);
				break;

			case 0x1E:
				parseReceivePing(msg);
				break;

			case 0x96:
				parseSay(msg);
				break;

			default:
				sendCancelWalk();
				break;
		}

		if(msg.isOverrun())
			disconnect();

		return;
	}

	switch(recvbyte)
	{
		case 0x14: // logout
			parseLogout(msg);
			break;

		case 0x1D: // keep alive / ping response
			parseReceivePingBack(msg);
			break;

		case 0x1E:
			parseReceivePing(msg);
			break;

		case 0x64: // move with steps
			parseAutoWalk(msg);
			break;

		case 0x65: // move north
		case 0x66: // move east
		case 0x67: // move south
		case 0x68: // move west
			parseMove(msg, (Direction)(recvbyte - 0x65));
			break;

		case 0x69: // stop-autowalk
			addGameTask(&Game::playerStopAutoWalk, player->getID());
			break;

		case 0x6A:
			parseMove(msg, NORTHEAST);
			break;

		case 0x6B:
			parseMove(msg, SOUTHEAST);
			break;

		case 0x6C:
			parseMove(msg, SOUTHWEST);
			break;

		case 0x6D:
			parseMove(msg, NORTHWEST);
			break;

		case 0x6F: // turn north
		case 0x70: // turn east
		case 0x71: // turn south
		case 0x72: // turn west
			parseTurn(msg, (Direction)(recvbyte - 0x6F));
			break;

		case 0x78: // throw item
			parseThrow(msg);
			break;

		case 0x79: // description in shop window
			parseLookInShop(msg);
			break;

		case 0x7A: // player bought from shop
			parsePlayerPurchase(msg);
			break;

		case 0x7B: // player sold to shop
			parsePlayerSale(msg);
			break;

		case 0x7C: // player closed shop window
			parseCloseShop(msg);
			break;

		case 0x7D: // Request trade
			parseRequestTrade(msg);
			break;

		case 0x7E: // Look at an item in trade
			parseLookInTrade(msg);
			break;

		case 0x7F: // Accept trade
			parseAcceptTrade(msg);
			break;

		case 0x80: // close/cancel trade
			parseCloseTrade();
			break;

		case 0x82: // use item
			parseUseItem(msg);
			break;

		case 0x83: // use item
			parseUseItemEx(msg);
			break;

		case 0x84: // battle window
			parseBattleWindow(msg);
			break;

		case 0x85: //rotate item
			parseRotateItem(msg);
			break;

		case 0x87: // close container
			parseCloseContainer(msg);
			break;

		case 0x88: //"up-arrow" - container
			parseUpArrowContainer(msg);
			break;

		case 0x89:
			parseTextWindow(msg);
			break;

		case 0x8A:
			parseHouseWindow(msg);
			break;

		case 0x8C:
			parseLookAt(msg);
			break;

		case 0x8D:
			parseLookInBattleList(msg);
			break;

		case 0x96: // say something
			parseSay(msg);
			break;

		case 0x97: // request channels
			parseGetChannels(msg);
			break;

		case 0x98: // open channel
			parseOpenChannel(msg);
			break;

		case 0x99: // close channel
			parseCloseChannel(msg);
			break;

		case 0x9A: // open priv
			parseOpenPrivateChannel(msg);
			break;

		case 0x9E: // close NPC
			parseCloseNpc(msg);
			break;

		case 0xA0: // set attack and follow mode
			parseFightModes(msg);
			break;

		case 0xA1: // attack
			parseAttack(msg);
			break;

		case 0xA2: //follow
			parseFollow(msg);
			break;

		case 0xA3: // invite party
			parseInviteToParty(msg);
			break;

		case 0xA4: // join party
			parseJoinParty(msg);
			break;

		case 0xA5: // revoke party
			parseRevokePartyInvite(msg);
			break;

		case 0xA6: // pass leadership
			parsePassPartyLeadership(msg);
			break;

		case 0xA7: // leave party
			parseLeaveParty(msg);
			break;

		case 0xA8: // share exp
			parseEnableSharedPartyExperience(msg);
			break;

		case 0xAA:
			parseCreatePrivateChannel(msg);
			break;

		case 0xAB:
			parseChannelInvite(msg);
			break;

		case 0xAC:
			parseChannelExclude(msg);
			break;

		case 0xBE: // cancel move
			parseCancelMove(msg);
			break;

		case 0xC9: //client request to resend the tile
			parseUpdateTile(msg);
			break;

		case 0xCA: //client request to resend the container (happens when you store more than container maxsize)
			parseUpdateContainer(msg);
			break;

		case 0xD2: // request outfit
			parseRequestOutfit(msg);
			break;

		case 0xD3: // set outfit
			parseSetOutfit(msg);
			break;

		case 0xD4:
			parseToggleMount(msg);
			break;

		case 0xDC:
			parseAddVip(msg);
			break;

		case 0xDD:
			parseRemoveVip(msg);
			break;

		case 0xE6:
			parseBugReport(msg);
			break;

		case 0xE8:
			parseDebugAssert(msg);
			break;

		case 0xF0:
			parseQuestLog(msg);
			break;

		case 0xF1:
			parseQuestLine(msg);
			break;

		case 0xF2:
			parseRuleViolationReport(msg);
			break;

		case 0xF4:
			parseMarketLeave();
			break;

		case 0xF5:
			parseMarketBrowse(msg);
			break;

		case 0xF6:
			parseMarketCreateOffer(msg);
			break;

		case 0xF7:
			parseMarketCancelOffer(msg);
			break;

		case 0xF8:
			parseMarketAcceptOffer(msg);
			break;

		default:
			std::cout << "Player: " << player->getName() << " sent an unknown packet header: 0x" << std::hex << (int16_t)recvbyte << std::dec << "!" << std::endl;
			break;
	}

	if(msg.isOverrun())
		disconnect();
}

void ProtocolGame::GetTileDescription(const Tile* tile, NetworkMessage_ptr msg)
{
	msg->AddU16(0x00); //environmental effects

	int32_t count = 0;
	if(tile->ground)
	{
		msg->AddItem(tile->ground);
		count++;
	}

	const TileItemVector* items = tile->getItemList();
	const CreatureVector* creatures = tile->getCreatures();

	ItemVector::const_iterator it;
	if(items)
	{
		for(it = items->getBeginTopItem(); ((it != items->getEndTopItem()) && (count < 10)); ++it)
		{
			msg->AddItem(*it);
			count++;
		}
	}

	if(creatures)
	{
		CreatureVector::const_reverse_iterator cit;
		for(cit = creatures->rbegin(); ((cit != creatures->rend()) && (count < 10)); ++cit)
		{
			if((*cit)->isInGhostMode() && !player->isAccessPlayer())
				continue;

			bool known;
			uint32_t removedKnown;
			checkCreatureAsKnown((*cit)->getID(), known, removedKnown);
			AddCreature(msg,*cit, known, removedKnown);
			count++;
		}
	}

	if(items)
	{
		for(it = items->getBeginDownItem(); ((it != items->getEndDownItem()) && (count < 10)); ++it)
		{
			msg->AddItem(*it);
			count++;
		}
	}
}

void ProtocolGame::GetMapDescription(int32_t x, int32_t y, int32_t z,
	int32_t width, int32_t height, NetworkMessage_ptr msg)
{
	int32_t skip = -1;
	int32_t startz, endz, zstep = 0;
	if(z > 7)
	{
		startz = z - 2;
		endz = std::min((int32_t)MAP_MAX_LAYERS - 1, (int32_t)z + 2);
		zstep = 1;
	}
	else
	{
		startz = 7;
		endz = 0;
		zstep = -1;
	}

	for(int32_t nz = startz; nz != endz + zstep; nz += zstep)
		GetFloorDescription(msg, x, y, nz, width, height, z - nz, skip);

	if(skip >= 0)
	{
		msg->AddByte(skip);
		msg->AddByte(0xFF);
	}

	#ifdef __DEBUG__
	//printf("tiles in total: %d \n", cc);
	#endif
}

void ProtocolGame::GetFloorDescription(NetworkMessage_ptr msg, int32_t x, int32_t y, int32_t z,
	int32_t width, int32_t height, int32_t offset, int32_t& skip)
{
	Tile* tile;
	for(int32_t nx = 0; nx < width; nx++)
	{
		for(int32_t ny = 0; ny < height; ny++)
		{
			tile = g_game.getTile(x + nx + offset, y + ny + offset, z);
			if(tile)
			{
				if(skip >= 0)
				{
					msg->AddByte(skip);
					msg->AddByte(0xFF);
				}
				skip = 0;
				GetTileDescription(tile, msg);
			}
			else
			{
				skip++;
				if(skip == 0xFF)
				{
					msg->AddByte(0xFF);
					msg->AddByte(0xFF);
					skip = -1;
				}
			}
		}
	}
}

void ProtocolGame::checkCreatureAsKnown(uint32_t id, bool &known, uint32_t &removedKnown)
{
	// loop through the known creature list and check if the given creature is in
	std::list<uint32_t>::iterator i;
	for(i = knownCreatureList.begin(); i != knownCreatureList.end(); ++i)
	{
		if((*i) == id)
		{
			// know... make the creature even more known...
			knownCreatureList.erase(i);
			knownCreatureList.push_back(id);

			known = true;
			return;
		}
	}

	// ok, he is unknown...
	known = false;

	// ... but not in future
	knownCreatureList.push_back(id);

	// too many known creatures?
	if(knownCreatureList.size() > 1300)
	{
		// lets try to remove one from the end of the list
		for(int32_t n = 0; n < 1300; n++)
		{
			removedKnown = knownCreatureList.front();

			Creature* c = g_game.getCreatureByID(removedKnown);
			if((!c) || (!canSee(c)))
				break;

			// this creature we can't remove, still in sight, so back to the end
			knownCreatureList.pop_front();
			knownCreatureList.push_back(removedKnown);
		}

		// hopefully we found someone to remove :S, we got only 250 tries
		// if not... lets kick some players with debug errors :)
		knownCreatureList.pop_front();
	}
	else
	{
		// we can cache without problems :)
		removedKnown = 0;
	}
}

bool ProtocolGame::canSee(const Creature* c) const
{
	if(!player || !c || c->isRemoved())
		return false;

	if(c->isInGhostMode() && !player->isAccessPlayer())
		return false;

	return canSee(c->getPosition());
}

bool ProtocolGame::canSee(const Position& pos) const
{
	return canSee(pos.x, pos.y, pos.z);
}

bool ProtocolGame::canSee(int32_t x, int32_t y, int32_t z) const
{
	if(!player)
		return false;

#ifdef __DEBUG__
	if(z < 0 || z >= MAP_MAX_LAYERS)
		std::cout << "WARNING! ProtocolGame::canSee() Z-value is out of range!" << std::endl;
#endif

	const Position& myPos = player->getPosition();
	if(myPos.z <= 7)
	{
		//we are on ground level or above (7 -> 0)
		//view is from 7 -> 0
		if(z > 7)
			return false;
	}
	else if(myPos.z >= 8)
	{
		//we are underground (8 -> 15)
		//view is +/- 2 from the floor we stand on
		if(std::abs(myPos.z - z) > 2)
			return false;
	}

	//negative offset means that the action taken place is on a lower floor than ourself
	int32_t offsetz = myPos.z - z;

	if((x >= myPos.x - 8 + offsetz) && (x <= myPos.x + 9 + offsetz) &&
		(y >= myPos.y - 6 + offsetz) && (y <= myPos.y + 7 + offsetz))
		return true;

	return false;
}

//********************** Parse methods *******************************//
void ProtocolGame::parseLogout(NetworkMessage& msg)
{
	g_dispatcher.addTask(createTask(boost::bind(&ProtocolGame::logout, this, true, false)));
}

void ProtocolGame::parseCreatePrivateChannel(NetworkMessage& msg)
{
	addGameTask(&Game::playerCreatePrivateChannel, player->getID());
}

void ProtocolGame::parseChannelInvite(NetworkMessage& msg)
{
	const std::string name = msg.GetString();
	addGameTask(&Game::playerChannelInvite, player->getID(), name);
}

void ProtocolGame::parseChannelExclude(NetworkMessage& msg)
{
	const std::string name = msg.GetString();
	addGameTask(&Game::playerChannelExclude, player->getID(), name);
}

void ProtocolGame::parseGetChannels(NetworkMessage& msg)
{
	addGameTask(&Game::playerRequestChannels, player->getID());
}

void ProtocolGame::parseOpenChannel(NetworkMessage& msg)
{
	uint16_t channelId = msg.GetU16();
	addGameTask(&Game::playerOpenChannel, player->getID(), channelId);
}

void ProtocolGame::parseCloseChannel(NetworkMessage& msg)
{
	uint16_t channelId = msg.GetU16();
	addGameTask(&Game::playerCloseChannel, player->getID(), channelId);
}

void ProtocolGame::parseOpenPrivateChannel(NetworkMessage& msg)
{
	const std::string receiver = msg.GetString();
	addGameTask(&Game::playerOpenPrivateChannel, player->getID(), receiver);
}

void ProtocolGame::parseCloseNpc(NetworkMessage& msg)
{
	addGameTask(&Game::playerCloseNpcChannel, player->getID());
}

void ProtocolGame::parseCancelMove(NetworkMessage& msg)
{
	addGameTask(&Game::playerCancelAttackAndFollow, player->getID());
}

void ProtocolGame::parseReceivePing(NetworkMessage& msg)
{
	addGameTask(&Game::playerReceivePing, player->getID());
}

void ProtocolGame::parseReceivePingBack(NetworkMessage& msg)
{
	addGameTask(&Game::playerReceivePingBack, player->getID());
}

void ProtocolGame::parseAutoWalk(NetworkMessage& msg)
{
	// first we get all directions...
	std::list<Direction> path;
	size_t numdirs = msg.GetByte();
	for(size_t i = 0; i < numdirs; ++i)
	{
		uint8_t rawdir = msg.GetByte();
		switch(rawdir)
		{
			case 1:
				path.push_back(EAST);
				break;
			case 2:
				path.push_back(NORTHEAST);
				break;
			case 3:
				path.push_back(NORTH);
				break;
			case 4:
				path.push_back(NORTHWEST);
				break;
			case 5:
				path.push_back(WEST);
				break;
			case 6:
				path.push_back(SOUTHWEST);
				break;
			case 7:
				path.push_back(SOUTH);
				break;
			case 8:
				path.push_back(SOUTHEAST);
				break;
			default:
				break;
		}
	}
	addGameTask(&Game::playerAutoWalk, player->getID(), path);
}

void ProtocolGame::parseMove(NetworkMessage& msg, Direction dir)
{
	addGameTask(&Game::playerMove, player->getID(), dir);
}

void ProtocolGame::parseTurn(NetworkMessage& msg, Direction dir)
{
	addGameTaskTimed(DISPATCHER_TASK_EXPIRATION, &Game::playerTurn, player->getID(), dir);
}

void ProtocolGame::parseRequestOutfit(NetworkMessage& msg)
{
	addGameTask(&Game::playerRequestOutfit, player->getID());
}

void ProtocolGame::parseSetOutfit(NetworkMessage& msg)
{
	Outfit_t newOutfit;
	newOutfit.lookType = msg.GetU16();
	newOutfit.lookHead = msg.GetByte();
	newOutfit.lookBody = msg.GetByte();
	newOutfit.lookLegs = msg.GetByte();
	newOutfit.lookFeet = msg.GetByte();
	newOutfit.lookAddons = msg.GetByte();
	newOutfit.lookMount = msg.GetU16();
	addGameTask(&Game::playerChangeOutfit, player->getID(), newOutfit);
}

void ProtocolGame::parseToggleMount(NetworkMessage& msg)
{
	bool mount = msg.GetByte() != 0;
	addGameTask(&Game::playerToggleMount, player->getID(), mount);
}

void ProtocolGame::parseUseItem(NetworkMessage& msg)
{
	Position pos = msg.GetPosition();
	uint16_t spriteId = msg.GetSpriteId();
	uint8_t stackpos = msg.GetByte();
	uint8_t index = msg.GetByte();
	bool isHotkey = (pos.x == 0xFFFF && pos.y == 0 && pos.z == 0);
	addGameTaskTimed(DISPATCHER_TASK_EXPIRATION, &Game::playerUseItem, player->getID(), pos, stackpos, index, spriteId, isHotkey);
}

void ProtocolGame::parseUseItemEx(NetworkMessage& msg)
{
	Position fromPos = msg.GetPosition();
	uint16_t fromSpriteId = msg.GetSpriteId();
	uint8_t fromStackPos = msg.GetByte();
	Position toPos = msg.GetPosition();
	uint16_t toSpriteId = msg.GetU16();
	uint8_t toStackPos = msg.GetByte();
	bool isHotkey = (fromPos.x == 0xFFFF && fromPos.y == 0 && fromPos.z == 0);
	addGameTaskTimed(DISPATCHER_TASK_EXPIRATION, &Game::playerUseItemEx, player->getID(), fromPos, fromStackPos, fromSpriteId, toPos, toStackPos, toSpriteId, isHotkey);
}

void ProtocolGame::parseBattleWindow(NetworkMessage& msg)
{
	Position fromPos = msg.GetPosition();
	uint16_t spriteId = msg.GetSpriteId();
	uint8_t fromStackPos = msg.GetByte();
	uint32_t creatureId = msg.GetU32();
	bool isHotkey = (fromPos.x == 0xFFFF && fromPos.y == 0 && fromPos.z == 0);
	addGameTaskTimed(DISPATCHER_TASK_EXPIRATION, &Game::playerUseBattleWindow, player->getID(), fromPos, fromStackPos, creatureId, spriteId, isHotkey);
}

void ProtocolGame::parseCloseContainer(NetworkMessage& msg)
{
	uint8_t cid = msg.GetByte();
	addGameTask(&Game::playerCloseContainer, player->getID(), cid);
}

void ProtocolGame::parseUpArrowContainer(NetworkMessage& msg)
{
	uint8_t cid = msg.GetByte();
	addGameTask(&Game::playerMoveUpContainer, player->getID(), cid);
}

void ProtocolGame::parseUpdateTile(NetworkMessage& msg)
{
	//Position pos = msg.GetPosition();
	//addGameTask(&Game::playerUpdateTile, player->getID(), pos);
}

void ProtocolGame::parseUpdateContainer(NetworkMessage& msg)
{
	uint8_t cid = msg.GetByte();
	addGameTask(&Game::playerUpdateContainer, player->getID(), cid);
}

void ProtocolGame::parseThrow(NetworkMessage& msg)
{
	Position fromPos = msg.GetPosition();
	uint16_t spriteId = msg.GetSpriteId();
	uint8_t fromStackpos = msg.GetByte();
	Position toPos = msg.GetPosition();
	uint8_t count = msg.GetByte();
	if(toPos != fromPos)
		addGameTaskTimed(DISPATCHER_TASK_EXPIRATION, &Game::playerMoveThing, player->getID(), fromPos, spriteId, fromStackpos, toPos, count);
}

void ProtocolGame::parseLookAt(NetworkMessage& msg)
{
	Position pos = msg.GetPosition();
	uint16_t spriteId = msg.GetSpriteId();
	uint8_t stackpos = msg.GetByte();
	addGameTaskTimed(DISPATCHER_TASK_EXPIRATION, &Game::playerLookAt, player->getID(), pos, spriteId, stackpos);
}

void ProtocolGame::parseLookInBattleList(NetworkMessage& msg)
{
	uint32_t creatureId = msg.GetU32();
	addGameTaskTimed(DISPATCHER_TASK_EXPIRATION, &Game::playerLookInBattleList, player->getID(), creatureId);
}

void ProtocolGame::parseSay(NetworkMessage& msg)
{
	SpeakClasses type = (SpeakClasses)msg.GetByte();

	std::string receiver;
	uint16_t channelId = 0;
	switch(type)
	{
		case SPEAK_PRIVATE_TO:
		case SPEAK_PRIVATE_RED_TO:
			receiver = msg.GetString();
			break;

		case SPEAK_CHANNEL_Y:
		case SPEAK_CHANNEL_R1:
			channelId = msg.GetU16();
			break;

		default:
			break;
	}

	const std::string text = msg.GetString();
	if(text.length() > 255)
		return;

	addGameTask(&Game::playerSay, player->getID(), channelId, type, receiver, text);
}

void ProtocolGame::parseFightModes(NetworkMessage& msg)
{
	uint8_t rawFightMode = msg.GetByte(); //1 - offensive, 2 - balanced, 3 - defensive
	uint8_t rawChaseMode = msg.GetByte(); // 0 - stand while fightning, 1 - chase opponent
	uint8_t rawSecureMode = msg.GetByte(); // 0 - can't attack unmarked, 1 - can attack unmarked

	chaseMode_t chaseMode = CHASEMODE_STANDSTILL;
	if(rawChaseMode == 1)
		chaseMode = CHASEMODE_FOLLOW;

	fightMode_t fightMode = FIGHTMODE_ATTACK;
	if(rawFightMode == 2)
		fightMode = FIGHTMODE_BALANCED;
	else if(rawFightMode == 3)
		fightMode = FIGHTMODE_DEFENSE;

	secureMode_t secureMode = SECUREMODE_OFF;
	if(rawSecureMode == 1)
		secureMode = SECUREMODE_ON;

	addGameTask(&Game::playerSetFightModes, player->getID(), fightMode, chaseMode, secureMode);
}

void ProtocolGame::parseAttack(NetworkMessage& msg)
{
	uint32_t creatureId = msg.GetU32();
	// msg.GetU32(); seq
	addGameTask(&Game::playerSetAttackedCreature, player->getID(), creatureId);
}

void ProtocolGame::parseFollow(NetworkMessage& msg)
{
	uint32_t creatureId = msg.GetU32();
	// msg.GetU32(); seq
	addGameTask(&Game::playerFollowCreature, player->getID(), creatureId);
}

void ProtocolGame::parseTextWindow(NetworkMessage& msg)
{
	uint32_t windowTextId = msg.GetU32();
	const std::string newText = msg.GetString();
	addGameTask(&Game::playerWriteItem, player->getID(), windowTextId, newText);
}

void ProtocolGame::parseHouseWindow(NetworkMessage &msg)
{
	uint8_t doorId = msg.GetByte();
	uint32_t id = msg.GetU32();
	const std::string text = msg.GetString();
	addGameTask(&Game::playerUpdateHouseWindow, player->getID(), doorId, id, text);
}

void ProtocolGame::parseLookInShop(NetworkMessage &msg)
{
	uint16_t id = msg.GetU16();
	uint8_t count = msg.GetByte();
	addGameTaskTimed(DISPATCHER_TASK_EXPIRATION, &Game::playerLookInShop, player->getID(), id, count);
}

void ProtocolGame::parsePlayerPurchase(NetworkMessage &msg)
{
	uint16_t id = msg.GetU16();
	uint8_t count = msg.GetByte();
	uint8_t amount = msg.GetByte();
	bool ignoreCap = msg.GetByte() == 0x01;
	bool inBackpacks = msg.GetByte() == 0x01;
	addGameTaskTimed(DISPATCHER_TASK_EXPIRATION, &Game::playerPurchaseItem, player->getID(), id, count, amount, ignoreCap, inBackpacks);
}

void ProtocolGame::parsePlayerSale(NetworkMessage &msg)
{
	uint16_t id = msg.GetU16();
	uint8_t count = msg.GetByte();
	uint8_t amount = msg.GetByte();
	addGameTaskTimed(DISPATCHER_TASK_EXPIRATION, &Game::playerSellItem, player->getID(), id, count, amount);
}

void ProtocolGame::parseCloseShop(NetworkMessage &msg)
{
	addGameTask(&Game::playerCloseShop, player->getID());
}

void ProtocolGame::parseRequestTrade(NetworkMessage& msg)
{
	Position pos = msg.GetPosition();
	uint16_t spriteId = msg.GetSpriteId();
	uint8_t stackpos = msg.GetByte();
	uint32_t playerId = msg.GetU32();
	addGameTask(&Game::playerRequestTrade, player->getID(), pos, stackpos, playerId, spriteId);
}

void ProtocolGame::parseAcceptTrade(NetworkMessage& msg)
{
	addGameTask(&Game::playerAcceptTrade, player->getID());
}

void ProtocolGame::parseLookInTrade(NetworkMessage& msg)
{
	bool counterOffer = (msg.GetByte() == 0x01);
	uint8_t index = msg.GetByte();
	addGameTaskTimed(DISPATCHER_TASK_EXPIRATION, &Game::playerLookInTrade, player->getID(), counterOffer, index);
}

void ProtocolGame::parseCloseTrade()
{
	addGameTask(&Game::playerCloseTrade, player->getID());
}

void ProtocolGame::parseAddVip(NetworkMessage& msg)
{
	const std::string name = msg.GetString();
	addGameTask(&Game::playerRequestAddVip, player->getID(), name);
}

void ProtocolGame::parseRemoveVip(NetworkMessage& msg)
{
	uint32_t guid = msg.GetU32();
	addGameTask(&Game::playerRequestRemoveVip, player->getID(), guid);
}

void ProtocolGame::parseRotateItem(NetworkMessage& msg)
{
	Position pos = msg.GetPosition();
	uint16_t spriteId = msg.GetSpriteId();
	uint8_t stackpos = msg.GetByte();
	addGameTaskTimed(DISPATCHER_TASK_EXPIRATION, &Game::playerRotateItem, player->getID(), pos, stackpos, spriteId);
}

void ProtocolGame::parseDebugAssert(NetworkMessage& msg)
{
	if(m_debugAssertSent)
		return;

	m_debugAssertSent = true;

	std::string assertLine = msg.GetString();
	std::string date = msg.GetString();
	std::string description = msg.GetString();
	std::string comment = msg.GetString();
	addGameTask(&Game::playerDebugAssert, player->getID(), assertLine, date, description, comment);
}

void ProtocolGame::parseBugReport(NetworkMessage& msg)
{
	std::string bug = msg.GetString();
	addGameTask(&Game::playerReportBug, player->getID(), bug);
}

void ProtocolGame::parseInviteToParty(NetworkMessage& msg)
{
	uint32_t targetId = msg.GetU32();
	addGameTask(&Game::playerInviteToParty, player->getID(), targetId);
}

void ProtocolGame::parseJoinParty(NetworkMessage& msg)
{
	uint32_t targetId = msg.GetU32();
	addGameTask(&Game::playerJoinParty, player->getID(), targetId);
}

void ProtocolGame::parseRevokePartyInvite(NetworkMessage& msg)
{
	uint32_t targetId = msg.GetU32();
	addGameTask(&Game::playerRevokePartyInvitation, player->getID(), targetId);
}

void ProtocolGame::parsePassPartyLeadership(NetworkMessage& msg)
{
	uint32_t targetId = msg.GetU32();
	addGameTask(&Game::playerPassPartyLeadership, player->getID(), targetId);
}

void ProtocolGame::parseLeaveParty(NetworkMessage& msg)
{
	addGameTask(&Game::playerLeaveParty, player->getID());
}

void ProtocolGame::parseEnableSharedPartyExperience(NetworkMessage& msg)
{
	bool sharedExpActive = msg.GetByte() == 1;
	addGameTask(&Game::playerEnableSharedPartyExperience, player->getID(), sharedExpActive);
}

void ProtocolGame::parseQuestLog(NetworkMessage& msg)
{
	addGameTaskTimed(DISPATCHER_TASK_EXPIRATION, &Game::playerShowQuestLog, player->getID());
}

void ProtocolGame::parseQuestLine(NetworkMessage& msg)
{
	uint16_t questId = msg.GetU16();
	addGameTask(&Game::playerShowQuestLine, player->getID(), questId);
}

void ProtocolGame::parseRuleViolationReport(NetworkMessage& msg)
{
	/* TODO
	ReportType_t type = msg.GetByte();
	uint8_t reason = msg.GetByte();
	std::string name = msg.GetString();
	std::string comment = msg.GetString();

	switch(type)
	{
		case REPORTTYPE_STATEMENT:
		{
			std::string translation = msg.GetString();
			uint32_t statementId = msg.GetU32();
			break;
		}

		case REPORTTYPE_NAME:
		{
			std::string translation = msg.GetString();
			break;
		}

		default: break;
	}
	*/
}

void ProtocolGame::parseMarketLeave()
{
	addGameTask(&Game::playerLeaveMarket, player->getID());
}

void ProtocolGame::parseMarketBrowse(NetworkMessage& msg)
{
	uint16_t browseId = msg.GetU16();
	if(browseId == MARKETREQUEST_OWN_OFFERS)
		addGameTask(&Game::playerBrowseMarketOwnOffers, player->getID());
	else if(browseId == MARKETREQUEST_OWN_HISTORY)
		addGameTask(&Game::playerBrowseMarketOwnHistory, player->getID());
	else
		addGameTask(&Game::playerBrowseMarket, player->getID(), browseId);
}

void ProtocolGame::parseMarketCreateOffer(NetworkMessage& msg)
{
	uint8_t type = msg.GetByte();
	uint16_t spriteId = msg.GetU16();
	uint16_t amount = msg.GetU16();
	uint32_t price = msg.GetU32();
	bool anonymous = (msg.GetByte() != 0);
	addGameTask(&Game::playerCreateMarketOffer, player->getID(), type, spriteId, amount, price, anonymous);
}

void ProtocolGame::parseMarketCancelOffer(NetworkMessage& msg)
{
	uint32_t timestamp = msg.GetU32();
	uint16_t counter = msg.GetU16();
	addGameTask(&Game::playerCancelMarketOffer, player->getID(), timestamp, counter);
}

void ProtocolGame::parseMarketAcceptOffer(NetworkMessage& msg)
{
	uint32_t timestamp = msg.GetU32();
	uint16_t counter = msg.GetU16();
	uint16_t amount = msg.GetU16();
	addGameTask(&Game::playerAcceptMarketOffer, player->getID(), timestamp, counter, amount);
}

//********************** Send methods *******************************//
void ProtocolGame::sendOpenPrivateChannel(const std::string& receiver)
{
	NetworkMessage_ptr msg = getOutputBuffer();
	if(!msg)
		return;

	TRACK_MESSAGE(msg);
	msg->AddByte(0xAD);
	msg->AddString(receiver);
}

void ProtocolGame::sendChannelEvent(uint16_t channelId, const std::string& playerName, ChannelEvent_t channelEvent)
{
	NetworkMessage_ptr msg = getOutputBuffer();
	if(!msg)
		return;

	TRACK_MESSAGE(msg);
	msg->AddByte(0xF3);
	msg->AddU16(channelId);
	msg->AddString(playerName);
	msg->AddByte(channelEvent);
}

void ProtocolGame::sendCreatureOutfit(const Creature* creature, const Outfit_t& outfit)
{
	if(!canSee(creature))
		return;

	NetworkMessage_ptr msg = getOutputBuffer();
	if(!msg)
		return;

	TRACK_MESSAGE(msg);
	msg->AddByte(0x8E);
	msg->AddU32(creature->getID());
	if(creature->isInGhostMode())
		AddCreatureInvisible(msg, creature);
	else
		AddCreatureOutfit(msg, creature, outfit);
}

void ProtocolGame::sendCreatureInvisible(const Creature* creature)
{
	if(!canSee(creature))
		return;

	NetworkMessage_ptr msg = getOutputBuffer();
	if(!msg)
		return;

	TRACK_MESSAGE(msg);
	msg->AddByte(0x8E);
	msg->AddU32(creature->getID());
	AddCreatureInvisible(msg, creature);
}

void ProtocolGame::sendCreatureLight(const Creature* creature)
{
	if(!canSee(creature))
		return;

	NetworkMessage_ptr msg = getOutputBuffer();
	if(!msg)
		return;

	TRACK_MESSAGE(msg);
	AddCreatureLight(msg, creature);
}

void ProtocolGame::sendWorldLight(const LightInfo& lightInfo)
{
	NetworkMessage_ptr msg = getOutputBuffer();
	if(!msg)
		return;

	TRACK_MESSAGE(msg);
	AddWorldLight(msg, lightInfo);
}

void ProtocolGame::sendCreatureShield(const Creature* creature)
{
	if(!canSee(creature))
		return;

	NetworkMessage_ptr msg = getOutputBuffer();
	if(!msg)
		return;

	TRACK_MESSAGE(msg);
	msg->AddByte(0x91);
	msg->AddU32(creature->getID());
	msg->AddByte(player->getPartyShield(creature->getPlayer()));
}

void ProtocolGame::sendCreatureSkull(const Creature* creature)
{
	if(g_game.getWorldType() != WORLD_TYPE_PVP)
		return;

	if(!canSee(creature))
		return;

	NetworkMessage_ptr msg = getOutputBuffer();
	if(!msg)
		return;

	TRACK_MESSAGE(msg);
	msg->AddByte(0x90);
	msg->AddU32(creature->getID());
	msg->AddByte(player->getSkullClient(creature->getPlayer()));
}

void ProtocolGame::sendCreatureSquare(const Creature* creature, SquareColor_t color)
{
	if(!canSee(creature))
		return;

	NetworkMessage_ptr msg = getOutputBuffer();
	if(!msg)
		return;

	TRACK_MESSAGE(msg);
	msg->AddByte(0x86);
	msg->AddU32(creature->getID());
	msg->AddByte((uint8_t)color);
}

void ProtocolGame::sendTutorial(uint8_t tutorialId)
{
	NetworkMessage_ptr msg = getOutputBuffer();
	if(!msg)
		return;

	TRACK_MESSAGE(msg);
	msg->AddByte(0xDC);
	msg->AddByte(tutorialId);
}

void ProtocolGame::sendAddMarker(const Position& pos, uint8_t markType, const std::string& desc)
{
	NetworkMessage_ptr msg = getOutputBuffer();
	if(!msg)
		return;

	TRACK_MESSAGE(msg);
	msg->AddByte(0xDD);
	msg->AddPosition(pos);
	msg->AddByte(markType);
	msg->AddString(desc);
}

void ProtocolGame::sendReLoginWindow()
{
	NetworkMessage_ptr msg = getOutputBuffer();
	if(!msg)
		return;

	TRACK_MESSAGE(msg);
	msg->AddByte(0x28);
	msg->AddByte(0xFF);
}

void ProtocolGame::sendStats()
{
	NetworkMessage_ptr msg = getOutputBuffer();
	if(!msg)
		return;

	TRACK_MESSAGE(msg);
	AddPlayerStats(msg);
}

void ProtocolGame::sendBasicData()
{
	NetworkMessage_ptr msg = getOutputBuffer();
	if(!msg)
		return;

	TRACK_MESSAGE(msg);
	msg->AddByte(0x9F);

	msg->AddByte(player->isPremium() ? 0x01 : 0x00);
	msg->AddByte(player->getVocation()->getClientId());

	// known spells
	msg->AddU16(0x00); // size
	//foreach spell: msg->AddByte(spellId);
}

void ProtocolGame::sendTextMessage(MessageClasses mclass, const std::string& message, Position* pos/* = NULL*/, uint32_t value/* = 0*/, TextColor_t color/* = TEXTCOLOR_NONE*/)
{
	NetworkMessage_ptr msg = getOutputBuffer();
	if(!msg)
		return;

	TRACK_MESSAGE(msg);
	if(pos != NULL && (mclass == MSG_DAMAGE_DEALT || mclass == MSG_DAMAGE_RECEIVED || mclass == MSG_HEALED || mclass == MSG_EXPERIENCE || mclass == MSG_DAMAGE_OTHERS || mclass == MSG_HEALED_OTHERS || mclass == MSG_EXPERIENCE_OTHERS))
		AddTextMessageEx(msg, mclass, message, *pos, value, color);
	else
		AddTextMessage(msg, mclass, message);
}

void ProtocolGame::sendClosePrivate(uint16_t channelId)
{
	NetworkMessage_ptr msg = getOutputBuffer();
	if(!msg)
		return;

	TRACK_MESSAGE(msg);
	if(channelId == CHANNEL_GUILD || channelId == CHANNEL_PARTY)
		g_chat.removeUserFromChannel(player, channelId);

	msg->AddByte(0xB3);
	msg->AddU16(channelId);
}

void ProtocolGame::sendCreatePrivateChannel(uint16_t channelId, const std::string& channelName)
{
	NetworkMessage_ptr msg = getOutputBuffer();
	if(!msg)
		return;

	TRACK_MESSAGE(msg);
	msg->AddByte(0xB2);
	msg->AddU16(channelId);
	msg->AddString(channelName);

	msg->AddU16(0x01);
	msg->AddString(player->getName());

	msg->AddU16(0x00);
}

void ProtocolGame::sendChannelsDialog()
{
	NetworkMessage_ptr msg = getOutputBuffer();
	if(!msg)
		return;

	TRACK_MESSAGE(msg);
	ChannelList list;
	list = g_chat.getChannelList(player);
	msg->AddByte(0xAB);
	msg->AddByte(list.size()); //how many
	while(list.size())
	{
		ChatChannel* channel = list.front();
		list.pop_front();
		msg->AddU16(channel->getId());
		msg->AddString(channel->getName());
	}
}

void ProtocolGame::sendChannel(uint16_t channelId, const std::string& channelName)
{
	NetworkMessage_ptr msg = getOutputBuffer();
	if(!msg)
		return;

	TRACK_MESSAGE(msg);
	msg->AddByte(0xAC);
	msg->AddU16(channelId);
	msg->AddString(channelName);

	if(channelId != CHANNEL_GUILD && channelId != CHANNEL_PARTY && channelId != CHANNEL_PRIVATE)
	{
		msg->AddU16(0x00);
		msg->AddU16(0x00);
		return;
	}

	ChatChannel* channel = g_chat.getChannel(player, channelId);
	if(!channel)
	{
		msg->AddU16(0x00);
		msg->AddU16(0x00);
		return;
	}

	UsersMap channelUsers = channel->getUsers();
	msg->AddU16(channelUsers.size());
	for(UsersMap::iterator it = channelUsers.begin(); it != channelUsers.end(); ++it)
		msg->AddString(it->second->getName());

	PrivateChatChannel* privateChannel = dynamic_cast<PrivateChatChannel*>(channel);
	if(privateChannel)
	{
		InvitedMap invitedUsers = privateChannel->getInvitedUsers();
		msg->AddU16(invitedUsers.size());
		for(InvitedMap::iterator it = invitedUsers.begin(); it != invitedUsers.end(); ++it)
			msg->AddString(it->second->getName());
	}
	else
		msg->AddU16(0x00);
}

void ProtocolGame::sendIcons(int32_t icons)
{
	NetworkMessage_ptr msg = getOutputBuffer();
	if(!msg)
		return;

	TRACK_MESSAGE(msg);
	msg->AddByte(0xA2);
	msg->AddU16(icons);
}

void ProtocolGame::sendContainer(uint32_t cid, const Container* container, bool hasParent)
{
	NetworkMessage_ptr msg = getOutputBuffer();
	if(!msg)
		return;

	TRACK_MESSAGE(msg);
	msg->AddByte(0x6E);
	msg->AddByte(cid);

	msg->AddItem(container);
	msg->AddString(container->getName());
	msg->AddByte(container->capacity());
	msg->AddByte(hasParent ? 0x01 : 0x00);
	msg->AddByte(std::min((uint32_t)0xFF, container->size()));

	uint32_t i = 0;
	for(ItemList::const_iterator cit = container->getItems(), end = container->getEnd(); i < 0xFF && cit != end; ++cit, ++i)
		msg->AddItem(*cit);
}

void ProtocolGame::sendShop(Npc* npc, const ShopInfoList& itemList)
{
	NetworkMessage_ptr msg = getOutputBuffer();
	if(!msg)
		return;

	TRACK_MESSAGE(msg);
	msg->AddByte(0x7A);
	msg->AddString(npc->getName());
	msg->AddU16(std::min((size_t)0xFFFF, itemList.size()));

	uint32_t i = 0;
	for(ShopInfoList::const_iterator it = itemList.begin(), end = itemList.end(); i < 0xFFFF && it != end; ++it, ++i)
		AddShopItem(msg, *it);
}

void ProtocolGame::sendCloseShop()
{
	NetworkMessage_ptr msg = getOutputBuffer();
	if(!msg)
		return;

	TRACK_MESSAGE(msg);
	msg->AddByte(0x7C);
}

void ProtocolGame::sendSaleItemList(const std::list<ShopInfo>& shop)
{
	NetworkMessage_ptr msg = getOutputBuffer();
	if(!msg)
		return;

	TRACK_MESSAGE(msg);
	msg->AddByte(0x7B);
	msg->AddU32(g_game.getMoney(player));

	std::map<uint32_t, uint32_t> saleMap;

	if(shop.size() <= 5)
	{
		// For very small shops it's not worth it to create the complete map
		for(std::list<ShopInfo>::const_iterator it = shop.begin(); it != shop.end(); ++it)
		{
			const ShopInfo& sInfo = *it;
			if(sInfo.sellPrice > 0)
			{
				int8_t subtype = -1;
				const ItemType& it = Item::items[sInfo.itemId];
				if(it.hasSubType() && !it.stackable)
					subtype = (sInfo.subType == 0 ? -1 : sInfo.subType);

				uint32_t count = player->__getItemTypeCount(sInfo.itemId, subtype);
				if(count > 0)
					saleMap[sInfo.itemId] = count;
			}
		}
	}
	else
	{
		// Large shop, it's better to get a cached map of all item counts and use it
		// We need a temporary map since the finished map should only contain items
		// available in the shop
		std::map<uint32_t, uint32_t> tempSaleMap;
		player->__getAllItemTypeCount(tempSaleMap);

		// We must still check manually for the special items that require subtype matches
		// (That is, fluids such as potions etc., actually these items are very few since
		// health potions now use their own ID)
		for(std::list<ShopInfo>::const_iterator it = shop.begin(); it != shop.end(); ++it)
		{
			const ShopInfo& sInfo = *it;
			if(sInfo.sellPrice > 0)
			{
				int8_t subtype = -1;
				const ItemType& it = Item::items[sInfo.itemId];
				if(it.hasSubType() && !it.stackable)
					subtype = (sInfo.subType == 0 ? -1 : sInfo.subType);

				if(subtype != -1)
				{
					uint32_t count = subtype;
					if(!it.isFluidContainer() && !it.isSplash())
						count = player->__getItemTypeCount(sInfo.itemId, subtype); // This shop item requires extra checks

					if(count > 0)
						saleMap[sInfo.itemId] = count;
					else
						saleMap[sInfo.itemId] = 0; // No count if you include subtypes in the calculations
				}
				else
					saleMap[sInfo.itemId] = tempSaleMap[sInfo.itemId];
			}
		}
	}

	msg->AddByte(std::min((size_t)0xFF, saleMap.size()));

	uint32_t i = 0;
	for(std::map<uint32_t, uint32_t>::const_iterator it = saleMap.begin(), end = saleMap.end(); i < 0xFF && it != end; ++it, ++i)
	{
		msg->AddItemId(it->first);
		msg->AddByte(std::min((uint32_t)0xFF, it->second));
	}
}

void ProtocolGame::sendMarketEnter(uint32_t depotId)
{
	NetworkMessage_ptr msg = getOutputBuffer();
	if(!msg)
		return;

	TRACK_MESSAGE(msg);
	msg->AddByte(0xF6);
	msg->AddU32(std::min((uint64_t)0xFFFFFFFF, player->getBankBalance()));
	msg->AddByte(std::min((int32_t)0xFF, IOMarket::getInstance()->getPlayerOfferCount(player->getGUID())));

	Depot* depot = player->getDepot(depotId, false);
	if(!depot)
	{
		msg->AddU16(0x00);
		return;
	}

	player->setMarketDepotId(depotId);

	std::map<uint16_t, uint32_t> depotItems;
	std::list<Container*> containerList;
	containerList.push_back(depot->getChest());
	do
	{
		Container* container = containerList.front();
		containerList.pop_front();
		for(ItemList::const_iterator it = container->getItems(), end = container->getEnd(); it != end; ++it)
		{
			Item* item = *it;

			Container* c = item->getContainer();
			if(c && !c->empty())
			{
				containerList.push_back(c);
				continue;
			}

			const ItemType& itemType = Item::items[item->getID()];
			if(!itemType.ware)
				continue;

			if(!itemType.isRune() && item->getCharges() != itemType.charges)
				continue;

			if(item->getDuration() != itemType.decayTime)
				continue;

			depotItems[item->getID()] += Item::countByType(item, -1);
		}
	}
	while(!containerList.empty());

	msg->AddU16(std::min((size_t)0xFFFF, depotItems.size()));

	uint16_t i = 0;
	for(std::map<uint16_t, uint32_t>::const_iterator it = depotItems.begin(), end = depotItems.end(); it != end && i < 65535; ++it, ++i)
	{
		msg->AddItemId(it->first);
		msg->AddU16(std::min((uint32_t)0xFFFF, it->second));
	}
}

void ProtocolGame::sendMarketLeave()
{
	NetworkMessage_ptr msg = getOutputBuffer();
	if(!msg)
		return;

	TRACK_MESSAGE(msg);
	msg->AddByte(0xF7);
}

void ProtocolGame::sendMarketBrowseItem(uint16_t itemId, const MarketOfferList& buyOffers, const MarketOfferList& sellOffers)
{
	NetworkMessage_ptr msg = getOutputBuffer();
	if(!msg)
		return;

	TRACK_MESSAGE(msg);
	msg->AddByte(0xF9);
	msg->AddItemId(itemId);

	msg->AddU32(buyOffers.size());
	for(MarketOfferList::const_iterator it = buyOffers.begin(), end = buyOffers.end(); it != end; ++it)
	{
		msg->AddU32(it->timestamp);
		msg->AddU16(it->counter);
		msg->AddU16(it->amount);
		msg->AddU32(it->price);
		msg->AddString(it->playerName);
	}

	msg->AddU32(sellOffers.size());
	for(MarketOfferList::const_iterator it = sellOffers.begin(), end = sellOffers.end(); it != end; ++it)
	{
		msg->AddU32(it->timestamp);
		msg->AddU16(it->counter);
		msg->AddU16(it->amount);
		msg->AddU32(it->price);
		msg->AddString(it->playerName);
	}
}

void ProtocolGame::sendMarketAcceptOffer(MarketOfferEx offer)
{
	NetworkMessage_ptr msg = getOutputBuffer();
	if(!msg)
		return;

	TRACK_MESSAGE(msg);
	msg->AddByte(0xF9);
	msg->AddItemId(offer.itemId);

	if(offer.type == MARKETACTION_BUY)
	{
		msg->AddU32(0x01);
		msg->AddU32(offer.timestamp);
		msg->AddU16(offer.counter);
		msg->AddU16(offer.amount);
		msg->AddU32(offer.price);
		msg->AddString(offer.playerName);
		msg->AddU32(0x00);
	}
	else
	{
		msg->AddU32(0x00);
		msg->AddU32(0x01);
		msg->AddU32(offer.timestamp);
		msg->AddU16(offer.counter);
		msg->AddU16(offer.amount);
		msg->AddU32(offer.price);
		msg->AddString(offer.playerName);
	}
}

void ProtocolGame::sendMarketBrowseOwnOffers(const MarketOfferList& buyOffers, const MarketOfferList& sellOffers)
{
	NetworkMessage_ptr msg = getOutputBuffer();
	if(!msg)
		return;

	TRACK_MESSAGE(msg);
	msg->AddByte(0xF9);
	msg->AddU16(MARKETREQUEST_OWN_OFFERS);

	msg->AddU32(buyOffers.size());
	for(MarketOfferList::const_iterator it = buyOffers.begin(), end = buyOffers.end(); it != end; ++it)
	{
		msg->AddU32(it->timestamp);
		msg->AddU16(it->counter);
		msg->AddItemId(it->itemId);
		msg->AddU16(it->amount);
		msg->AddU32(it->price);
	}

	msg->AddU32(sellOffers.size());
	for(MarketOfferList::const_iterator it = sellOffers.begin(), end = sellOffers.end(); it != end; ++it)
	{
		msg->AddU32(it->timestamp);
		msg->AddU16(it->counter);
		msg->AddItemId(it->itemId);
		msg->AddU16(it->amount);
		msg->AddU32(it->price);
	}
}

void ProtocolGame::sendMarketCancelOffer(MarketOfferEx offer)
{
	NetworkMessage_ptr msg = getOutputBuffer();
	if(!msg)
		return;

	TRACK_MESSAGE(msg);
	msg->AddByte(0xF9);
	msg->AddU16(MARKETREQUEST_OWN_OFFERS);

	if(offer.type == MARKETACTION_BUY)
	{
		msg->AddU32(0x01);
		msg->AddU32(offer.timestamp);
		msg->AddU16(offer.counter);
		msg->AddItemId(offer.itemId);
		msg->AddU16(offer.amount);
		msg->AddU32(offer.price);
		msg->AddU32(0x00);
	}
	else
	{
		msg->AddU32(0x00);
		msg->AddU32(0x01);
		msg->AddU32(offer.timestamp);
		msg->AddU16(offer.counter);
		msg->AddItemId(offer.itemId);
		msg->AddU16(offer.amount);
		msg->AddU32(offer.price);
	}
}

void ProtocolGame::sendMarketBrowseOwnHistory(const HistoryMarketOfferList& buyOffers, const HistoryMarketOfferList& sellOffers)
{
	NetworkMessage_ptr msg = getOutputBuffer();
	if(!msg)
		return;

	TRACK_MESSAGE(msg);
	msg->AddByte(0xF9);
	msg->AddU16(MARKETREQUEST_OWN_HISTORY);

	std::map<uint32_t, uint16_t> counterMap;

	msg->AddU32(buyOffers.size());
	for(HistoryMarketOfferList::const_iterator it = buyOffers.begin(), end = buyOffers.end(); it != end; ++it)
	{
		msg->AddU32(it->timestamp);
		msg->AddU16(counterMap[it->timestamp]++);
		msg->AddItemId(it->itemId);
		msg->AddU16(it->amount);
		msg->AddU32(it->price);
		msg->AddByte(it->state);
	}

	counterMap.clear();

	msg->AddU32(sellOffers.size());
	for(HistoryMarketOfferList::const_iterator it = sellOffers.begin(), end = sellOffers.end(); it != end; ++it)
	{
		msg->AddU32(it->timestamp);
		msg->AddU16(counterMap[it->timestamp]++);
		msg->AddItemId(it->itemId);
		msg->AddU16(it->amount);
		msg->AddU32(it->price);
		msg->AddByte(it->state);
	}
}

void ProtocolGame::sendMarketDetail(uint16_t itemId)
{
	NetworkMessage_ptr msg = getOutputBuffer();
	if(!msg)
		return;

	TRACK_MESSAGE(msg);
	msg->AddByte(0xF8);
	msg->AddItemId(itemId);

	const ItemType& it = Item::items[itemId];
	std::ostringstream ss;
	if(it.armor != 0)
	{
		ss << it.armor;
		msg->AddString(ss.str());
	}
	else
		msg->AddU16(0x00);

	if(it.attack != 0)
	{
		ss.str("");
		ss << it.attack;
		if(it.abilities && it.abilities->elementType != COMBAT_NONE && it.decayTo > 0)
			ss << " physical +" << it.abilities->elementDamage << " " << getCombatName(it.abilities->elementType);

		msg->AddString(ss.str());
	}
	else
		msg->AddU16(0x00);

	if(it.isContainer())
	{
		ss.str("");
		ss << it.maxItems;
		msg->AddString(ss.str());
	}
	else
		msg->AddU16(0x00);

	if(it.defense != 0)
	{
		ss.str("");
		ss << it.defense;
		if(it.extraDefense != 0)
			ss << " " << std::showpos << it.extraDefense << std::noshowpos;

		msg->AddString(ss.str());
	}
	else
		msg->AddU16(0x00);

	if(!it.description.empty())
	{
		std::string descr = it.description;
		if(descr[descr.length() - 1] == '.')
			descr.erase(descr.length() - 1);

		msg->AddString(descr);
	}
	else
		msg->AddU16(0x00);

	if(it.decayTime != 0)
	{
		ss.str("");
		ss << it.decayTime << " seconds";
		msg->AddString(ss.str());
	}
	else
		msg->AddU16(0x00);

	ss.str("");
	if(it.abilities)
	{
		bool separator = false;
		for(uint32_t i = (COMBAT_FIRST + 1); i <= COMBAT_COUNT; ++i)
		{
			if(!it.abilities->absorbPercent[i])
				continue;

			if(separator)
				ss << ", ";
			else
				separator = true;

			ss << getCombatName(indexToCombatType(i)) << " " << std::showpos << it.abilities->absorbPercent[i] << std::noshowpos << "%";
		}
	}
	msg->AddString(ss.str());

	if(it.minReqLevel != 0)
	{
		ss.str("");
		ss << it.minReqLevel;
		msg->AddString(ss.str());
	}
	else
		msg->AddU16(0x00);

	if(it.minReqMagicLevel != 0)
	{
		ss.str("");
		ss << it.minReqMagicLevel;
		msg->AddString(ss.str());
	}
	else
		msg->AddU16(0x00);

	msg->AddString(it.vocationString);

	msg->AddString(it.runeSpellName);

	ss.str("");
	if(it.abilities)
	{
		bool separator = false;
		for(uint16_t i = SKILL_FIRST; i <= SKILL_LAST; i++)
		{
			if(!it.abilities->skills[i])
				continue;

			if(separator)
				ss << ", ";
			else
				separator = true;

			ss << getSkillName(i) << " " << std::showpos << it.abilities->skills[i] << std::noshowpos;
		}

		if(it.abilities->stats[STAT_MAGICPOINTS] != 0)
		{
			if(separator)
				ss << ", ";
			else
				separator = true;

			ss << "magic level " << std::showpos << it.abilities->stats[STAT_MAGICPOINTS] << std::noshowpos;
		}

		if(it.abilities->speed != 0)
		{
			if(separator)
				ss << ", ";

			ss << "speed" << " " << std::showpos << (int32_t)(it.abilities->speed / 2) << std::noshowpos;
		}
	}
	msg->AddString(ss.str());

	if(it.charges != 0)
	{
		ss.str("");
		ss << it.charges;
		msg->AddString(ss.str());
	}
	else
		msg->AddU16(0x00);

	std::string weaponName = getWeaponName(it.weaponType);
	if(it.slotPosition & SLOTP_TWO_HAND)
	{
		if(!weaponName.empty())
			weaponName += ", two-handed";
		else
			weaponName = "two-handed";
	}
	msg->AddString(weaponName);

	if(it.weight > 0)
	{
		ss.str("");
		ss << std::fixed << std::setprecision(2) << it.weight << " oz";
		msg->AddString(ss.str());
	}
	else
		msg->AddU16(0x00);

	MarketStatistics* statistics = IOMarket::getInstance()->getPurchaseStatistics(itemId);
	if(statistics)
	{
		msg->AddByte(0x01);
		msg->AddU32(statistics->numTransactions);
		msg->AddU32(std::min((uint64_t)0xFFFFFFFF, statistics->totalPrice));
		msg->AddU32(statistics->highestPrice);
		msg->AddU32(statistics->lowestPrice);
	}
	else
		msg->AddByte(0x00);
	
	statistics = IOMarket::getInstance()->getSaleStatistics(itemId);
	if(statistics)
	{
		msg->AddByte(0x01);
		msg->AddU32(statistics->numTransactions);
		msg->AddU32(std::min((uint64_t)0xFFFFFFFF, statistics->totalPrice));
		msg->AddU32(statistics->highestPrice);
		msg->AddU32(statistics->lowestPrice);
	}
	else
		msg->AddByte(0x00);
}

void ProtocolGame::sendQuestLog()
{
	NetworkMessage_ptr msg = getOutputBuffer();
	if(!msg)
		return;

	TRACK_MESSAGE(msg);
	msg->AddByte(0xF0);
	msg->AddU16(Quests::getInstance()->getQuestsCount(player));
	for(QuestsList::const_iterator it = Quests::getInstance()->getFirstQuest(),
		end = Quests::getInstance()->getLastQuest(); it != end; ++it)
	{
		Quest* quest = *it;
		if(quest->isStarted(player))
		{
			msg->AddU16(quest->getID());
			msg->AddString(quest->getName());
			msg->AddByte(quest->isCompleted(player));
		}
	}
}

void ProtocolGame::sendQuestLine(const Quest* quest)
{
	NetworkMessage_ptr msg = getOutputBuffer();
	if(!msg)
		return;

	TRACK_MESSAGE(msg);
	msg->AddByte(0xF1);
	msg->AddU16(quest->getID());
	msg->AddByte(quest->getMissionsCount(player));
	for(MissionsList::const_iterator it = quest->getFirstMission(), end = quest->getLastMission();
		it != end; ++it)
	{
		Mission* mission = *it;
		if(mission->isStarted(player))
		{
			msg->AddString(mission->getName(player));
			msg->AddString(mission->getDescription(player));
		}
	}
}

void ProtocolGame::sendTradeItemRequest(const Player* player, const Item* item, bool ack)
{
	NetworkMessage_ptr msg = getOutputBuffer();
	if(!msg)
		return;

	TRACK_MESSAGE(msg);
	if(ack)
		msg->AddByte(0x7D);
	else
		msg->AddByte(0x7E);

	msg->AddString(player->getName());

	if(const Container* tradeContainer = item->getContainer())
	{
		std::list<const Container*> listContainer;
		ItemList::const_iterator it;
		Container* tmpContainer = NULL;

		listContainer.push_back(tradeContainer);

		std::list<const Item*> listItem;
		listItem.push_back(tradeContainer);
		while(!listContainer.empty())
		{
			const Container* container = listContainer.front();
			listContainer.pop_front();
			for(it = container->getItems(); it != container->getEnd(); ++it)
			{
				if((tmpContainer = (*it)->getContainer()))
					listContainer.push_back(tmpContainer);
				listItem.push_back(*it);
			}
		}

		msg->AddByte(listItem.size());
		while(!listItem.empty())
		{
			const Item* item = listItem.front();
			listItem.pop_front();
			msg->AddItem(item);
		}
	}
	else
	{
		msg->AddByte(0x01);
		msg->AddItem(item);
	}
}

void ProtocolGame::sendCloseTrade()
{
	NetworkMessage_ptr msg = getOutputBuffer();
	if(!msg)
		return;

	TRACK_MESSAGE(msg);
	msg->AddByte(0x7F);
}

void ProtocolGame::sendCloseContainer(uint32_t cid)
{
	NetworkMessage_ptr msg = getOutputBuffer();
	if(!msg)
		return;

	TRACK_MESSAGE(msg);
	msg->AddByte(0x6F);
	msg->AddByte(cid);
}

void ProtocolGame::sendCreatureTurn(const Creature* creature, uint32_t stackPos)
{
	if(stackPos >= 10 || !canSee(creature))
		return;

	NetworkMessage_ptr msg = getOutputBuffer();
	if(!msg)
		return;

	TRACK_MESSAGE(msg);
	msg->AddByte(0x6B);
	msg->AddPosition(creature->getPosition());
	msg->AddByte(stackPos);
	msg->AddU16(0x63); /*99*/
	msg->AddU32(creature->getID());
	msg->AddByte(creature->getDirection());
	msg->AddByte(player->isAccessPlayer() ? 0x00 : 0x01);
}

void ProtocolGame::sendCreatureSay(const Creature* creature, SpeakClasses type, const std::string& text, Position* pos/* = NULL*/)
{
	NetworkMessage_ptr msg = getOutputBuffer();
	if(!msg)
		return;

	TRACK_MESSAGE(msg);
	AddCreatureSpeak(msg, creature, type, text, 0, pos);
}

void ProtocolGame::sendToChannel(const Creature* creature, SpeakClasses type, const std::string& text, uint16_t channelId)
{
	NetworkMessage_ptr msg = getOutputBuffer();
	if(!msg)
		return;

	TRACK_MESSAGE(msg);
	AddCreatureSpeak(msg, creature, type, text, channelId);
}

void ProtocolGame::sendCancel(const std::string& message)
{
	NetworkMessage_ptr msg = getOutputBuffer();
	if(!msg)
		return;

	TRACK_MESSAGE(msg);
	AddTextMessage(msg, MSG_STATUS_SMALL, message);
}

void ProtocolGame::sendCancelTarget()
{
	NetworkMessage_ptr msg = getOutputBuffer();
	if(!msg)
		return;

	TRACK_MESSAGE(msg);
	msg->AddByte(0xA3);
	msg->AddU32(0x00);
}

void ProtocolGame::sendChangeSpeed(const Creature* creature, uint32_t speed)
{
	NetworkMessage_ptr msg = getOutputBuffer();
	if(!msg)
		return;

	TRACK_MESSAGE(msg);
	msg->AddByte(0x8F);
	msg->AddU32(creature->getID());
	msg->AddU16(speed);
}

void ProtocolGame::sendCancelWalk()
{
	NetworkMessage_ptr msg = getOutputBuffer();
	if(!msg)
		return;

	TRACK_MESSAGE(msg);
	msg->AddByte(0xB5);
	msg->AddByte(player->getDirection());
}

void ProtocolGame::sendSkills()
{
	NetworkMessage_ptr msg = getOutputBuffer();
	if(!msg)
		return;

	TRACK_MESSAGE(msg);
	AddPlayerSkills(msg);
}

void ProtocolGame::sendPing()
{
	NetworkMessage_ptr msg = getOutputBuffer();
	if(!msg)
		return;

	TRACK_MESSAGE(msg);
	msg->AddByte(0x1D);
}

void ProtocolGame::sendPingBack()
{
	NetworkMessage_ptr msg = getOutputBuffer();
	if(!msg)
		return;

	TRACK_MESSAGE(msg);
	msg->AddByte(0x1E);
}

void ProtocolGame::sendDistanceShoot(const Position& from, const Position& to, uint8_t type)
{
	if(type > NM_SHOOT_LAST || type == NM_SHOOT_UNK1 || type == NM_SHOOT_UNK2 || type == NM_SHOOT_UNK3)
		return;

	if(!canSee(from) && !canSee(to))
		return;

	NetworkMessage_ptr msg = getOutputBuffer();
	if(!msg)
		return;

	TRACK_MESSAGE(msg);
	AddDistanceShoot(msg, from, to, type);
}

void ProtocolGame::sendMagicEffect(const Position& pos, uint8_t type)
{
	if(type > NM_ME_LAST)
		return;

	if(!canSee(pos))
		return;

	NetworkMessage_ptr msg = getOutputBuffer();
	if(!msg)
		return;

	TRACK_MESSAGE(msg);
	AddMagicEffect(msg, pos, type);
}

void ProtocolGame::sendCreatureHealth(const Creature* creature)
{
	NetworkMessage_ptr msg = getOutputBuffer();
	if(!msg)
		return;

	TRACK_MESSAGE(msg);
	AddCreatureHealth(msg, creature);
}

void ProtocolGame::sendFYIBox(const std::string& message)
{
	NetworkMessage_ptr msg = getOutputBuffer();
	if(!msg)
		return;

	TRACK_MESSAGE(msg);
	msg->AddByte(0x15);
	msg->AddString(message);
}

//tile
void ProtocolGame::sendAddTileItem(const Tile* tile, const Position& pos, uint32_t stackpos, const Item* item)
{
	if(!canSee(pos))
		return;

	NetworkMessage_ptr msg = getOutputBuffer();
	if(!msg)
		return;

	TRACK_MESSAGE(msg);
	AddTileItem(msg, pos, stackpos, item);
}

void ProtocolGame::sendUpdateTileItem(const Tile* tile, const Position& pos, uint32_t stackpos, const Item* item)
{
	if(!canSee(pos))
		return;

	NetworkMessage_ptr msg = getOutputBuffer();
	if(!msg)
		return;

	TRACK_MESSAGE(msg);
	UpdateTileItem(msg, pos, stackpos, item);
}

void ProtocolGame::sendRemoveTileItem(const Tile* tile, const Position& pos, uint32_t stackpos)
{
	if(!canSee(pos))
		return;

	NetworkMessage_ptr msg = getOutputBuffer();
	if(!msg)
		return;

	TRACK_MESSAGE(msg);
	RemoveTileItem(msg, pos, stackpos);
}

void ProtocolGame::sendUpdateTile(const Tile* tile, const Position& pos)
{
	if(!canSee(pos))
		return;

	NetworkMessage_ptr msg = getOutputBuffer();
	if(!msg)
		return;

	TRACK_MESSAGE(msg);
	msg->AddByte(0x69);
	msg->AddPosition(pos);
	if(tile)
	{
		GetTileDescription(tile, msg);
		msg->AddByte(0x00);
		msg->AddByte(0xFF);
	}
	else
	{
		msg->AddByte(0x01);
		msg->AddByte(0xFF);
	}
}

void ProtocolGame::sendAddCreature(const Creature* creature, const Position& pos, uint32_t stackpos, bool isLogin)
{
	if(!canSee(pos))
		return;

	NetworkMessage_ptr msg = getOutputBuffer();
	if(!msg)
		return;

	TRACK_MESSAGE(msg);
	if(creature != player)
	{
		AddTileCreature(msg, pos, stackpos, creature);
		if(isLogin)
			AddMagicEffect(msg, pos, NM_ME_TELEPORT);

		return;
	}

	msg->AddByte(0x0A);
	msg->AddU32(player->getID());

	msg->AddU16(0x32);

	// can report bugs?
	if(player->getAccountType() >= ACCOUNT_TYPE_TUTOR)
		msg->AddByte(0x01);
	else
		msg->AddByte(0x00);

	AddMapDescription(msg, pos);

	if(isLogin)
		AddMagicEffect(msg, pos, NM_ME_TELEPORT);

	AddInventoryItem(msg, SLOT_HEAD, player->getInventoryItem(SLOT_HEAD));
	AddInventoryItem(msg, SLOT_NECKLACE, player->getInventoryItem(SLOT_NECKLACE));
	AddInventoryItem(msg, SLOT_BACKPACK, player->getInventoryItem(SLOT_BACKPACK));
	AddInventoryItem(msg, SLOT_ARMOR, player->getInventoryItem(SLOT_ARMOR));
	AddInventoryItem(msg, SLOT_RIGHT, player->getInventoryItem(SLOT_RIGHT));
	AddInventoryItem(msg, SLOT_LEFT, player->getInventoryItem(SLOT_LEFT));
	AddInventoryItem(msg, SLOT_LEGS, player->getInventoryItem(SLOT_LEGS));
	AddInventoryItem(msg, SLOT_FEET, player->getInventoryItem(SLOT_FEET));
	AddInventoryItem(msg, SLOT_RING, player->getInventoryItem(SLOT_RING));
	AddInventoryItem(msg, SLOT_AMMO, player->getInventoryItem(SLOT_AMMO));

	AddPlayerStats(msg);
	AddPlayerSkills(msg);

	//gameworld light-settings
	LightInfo lightInfo;
	g_game.getWorldLightInfo(lightInfo);
	AddWorldLight(msg, lightInfo);

	//player light level
	AddCreatureLight(msg, creature);

	if(isLogin)
	{
		AccountManager* accountManager = player->getAccountManager();
		if(accountManager)
		{
			if(accountManager->namelockedPlayerName != "")
				AddTextMessage(msg, MSG_STATUS_CONSOLE_ORANGE, "Hello, it appears that your character has been namelocked, what would you like as your new name?");
			else if(accountManager->managingAccount)
				AddTextMessage(msg, MSG_STATUS_CONSOLE_ORANGE, "Hello, type 'account' to manage your account and if you want to start over then type 'cancel'.");
			else
				AddTextMessage(msg, MSG_STATUS_CONSOLE_ORANGE, "Hello, type 'account' to create an account or type 'recover' to recover an account.");
		}
		else
		{
			std::string tmpStr = g_config.getString(ConfigManager::LOGIN_MSG);
			if(player->getLastLoginSaved() <= 0)
			{
				tmpStr += " Please choose your outfit.";
				sendOutfitWindow();
			}
			else
			{
				if(!tmpStr.empty())
					AddTextMessage(msg, MSG_STATUS_DEFAULT, tmpStr.c_str());

				time_t lastLogin = player->getLastLoginSaved();
				std::ostringstream ss;
				ss << "Your last visit was on " << ctime(&lastLogin);
				tmpStr = ss.str();
				tmpStr.erase(tmpStr.length() - 1);
				tmpStr += ".";
			}
			AddTextMessage(msg, MSG_STATUS_DEFAULT, tmpStr);
		}
	}

	for(VIPListSet::iterator it = player->VIPList.begin(), end = player->VIPList.end(); it != end; ++it)
	{
		std::string vip_name;
		if(IOLoginData::getInstance()->getNameByGuid((*it), vip_name))
		{
			Player* tmpPlayer = g_game.getPlayerByName(vip_name);
			sendVIP((*it), vip_name, (tmpPlayer && (!tmpPlayer->isInGhostMode() || player->isAccessPlayer())));
		}
	}
	player->sendIcons();
}

void ProtocolGame::sendRemoveCreature(const Creature* creature, const Position& pos, uint32_t stackpos, bool isLogout)
{
	if(creature->isInGhostMode() && !player->isAccessPlayer())
		return;

	if(!canSee(pos))
		return;

	NetworkMessage_ptr msg = getOutputBuffer();
	if(!msg)
		return;

	TRACK_MESSAGE(msg);
	RemoveTileItem(msg, pos, stackpos);
}

void ProtocolGame::sendMoveCreature(const Creature* creature, const Tile* newTile, const Position& newPos,
	uint32_t newStackPos, const Tile* oldTile, const Position& oldPos, uint32_t oldStackPos, bool teleport)
{
	if(creature == player)
	{
		NetworkMessage_ptr msg = getOutputBuffer();
		if(!msg)
			return;

		TRACK_MESSAGE(msg);
		if(teleport || oldStackPos >= 10)
		{
			RemoveTileItem(msg, oldPos, oldStackPos);
			AddMapDescription(msg, newPos);
		}
		else
		{
			if(oldPos.z == 7 && newPos.z >= 8)
				RemoveTileItem(msg, oldPos, oldStackPos);
			else
			{
				msg->AddByte(0x6D);
				msg->AddPosition(oldPos);
				msg->AddByte(oldStackPos);
				msg->AddPosition(newPos);
			}

			if(newPos.z > oldPos.z)
				MoveDownCreature(msg, creature, newPos, oldPos, oldStackPos);
			else if(newPos.z < oldPos.z)
				MoveUpCreature(msg, creature, newPos, oldPos, oldStackPos);

			if(oldPos.y > newPos.y) // north, for old x
			{
				msg->AddByte(0x65);
				GetMapDescription(oldPos.x - 8, newPos.y - 6, newPos.z, 18, 1, msg);
			}
			else if(oldPos.y < newPos.y) // south, for old x
			{
				msg->AddByte(0x67);
				GetMapDescription(oldPos.x - 8, newPos.y + 7, newPos.z, 18, 1, msg);
			}

			if(oldPos.x < newPos.x) // east, [with new y]
			{
				msg->AddByte(0x66);
				GetMapDescription(newPos.x + 9, newPos.y - 6, newPos.z, 1, 14, msg);
			}
			else if(oldPos.x > newPos.x) // west, [with new y]
			{
				msg->AddByte(0x68);
				GetMapDescription(newPos.x - 8, newPos.y - 6, newPos.z, 1, 14, msg);
			}
		}
	}
	else if(canSee(oldPos) && canSee(creature->getPosition()))
	{	
		if(teleport || (oldPos.z == 7 && newPos.z >= 8) || oldStackPos >= 10)
		{
			sendRemoveCreature(creature, oldPos, oldStackPos, false);
			sendAddCreature(creature, newPos, newStackPos, false);
		}
		else
		{
			NetworkMessage_ptr msg = getOutputBuffer();
			if(!msg)
				return;

			TRACK_MESSAGE(msg);
			msg->AddByte(0x6D);
			msg->AddPosition(oldPos);
			msg->AddByte(oldStackPos);
			msg->AddPosition(creature->getPosition());
		}
	}
	else if(canSee(oldPos))
		sendRemoveCreature(creature, oldPos, oldStackPos, false);
	else if(canSee(creature->getPosition()))
		sendAddCreature(creature, newPos, newStackPos, false);
}

//inventory
void ProtocolGame::sendAddInventoryItem(slots_t slot, const Item* item)
{
	NetworkMessage_ptr msg = getOutputBuffer();
	if(!msg)
		return;

	TRACK_MESSAGE(msg);
	AddInventoryItem(msg, slot, item);
}

void ProtocolGame::sendUpdateInventoryItem(slots_t slot, const Item* item)
{
	NetworkMessage_ptr msg = getOutputBuffer();
	if(!msg)
		return;

	TRACK_MESSAGE(msg);
	UpdateInventoryItem(msg, slot, item);
}

void ProtocolGame::sendRemoveInventoryItem(slots_t slot)
{
	NetworkMessage_ptr msg = getOutputBuffer();
	if(!msg)
		return;

	TRACK_MESSAGE(msg);
	RemoveInventoryItem(msg, slot);
}

//containers
void ProtocolGame::sendAddContainerItem(uint8_t cid, const Item* item)
{
	NetworkMessage_ptr msg = getOutputBuffer();
	if(!msg)
		return;

	TRACK_MESSAGE(msg);
	AddContainerItem(msg, cid, item);
}

void ProtocolGame::sendUpdateContainerItem(uint8_t cid, uint8_t slot, const Item* item)
{
	NetworkMessage_ptr msg = getOutputBuffer();
	if(!msg)
		return;

	TRACK_MESSAGE(msg);
	UpdateContainerItem(msg, cid, slot, item);
}

void ProtocolGame::sendRemoveContainerItem(uint8_t cid, uint8_t slot)
{
	NetworkMessage_ptr msg = getOutputBuffer();
	if(!msg)
		return;

	TRACK_MESSAGE(msg);
	RemoveContainerItem(msg, cid, slot);
}

void ProtocolGame::sendTextWindow(uint32_t windowTextId, Item* item, uint16_t maxlen, bool canWrite)
{
	NetworkMessage_ptr msg = getOutputBuffer();
	if(!msg)
		return;

	TRACK_MESSAGE(msg);
	msg->AddByte(0x96);
	msg->AddU32(windowTextId);
	msg->AddItem(item);
	if(canWrite)
	{
		msg->AddU16(maxlen);
		msg->AddString(item->getText());
	}
	else
	{
		msg->AddU16(item->getText().size());
		msg->AddString(item->getText());
	}

	const std::string& writer = item->getWriter();
	if(writer.size())
		msg->AddString(writer);
	else
		msg->AddU16(0x00);

	time_t writtenDate = item->getDate();
	if(writtenDate > 0)
		msg->AddString(formatDateShort(writtenDate));
	else
		msg->AddU16(0x00);
}

void ProtocolGame::sendTextWindow(uint32_t windowTextId, uint32_t itemId, const std::string& text)
{
	NetworkMessage_ptr msg = getOutputBuffer();
	if(!msg)
		return;

	TRACK_MESSAGE(msg);
	msg->AddByte(0x96);
	msg->AddU32(windowTextId);
	msg->AddItem(itemId, 1);

	msg->AddU16(text.size());
	msg->AddString(text);

	msg->AddU16(0x00);
	msg->AddU16(0x00);
}

void ProtocolGame::sendHouseWindow(uint32_t windowTextId, House* _house,
	uint32_t listId, const std::string& text)
{
	NetworkMessage_ptr msg = getOutputBuffer();
	if(!msg)
		return;

	TRACK_MESSAGE(msg);
	msg->AddByte(0x97);
	msg->AddByte(0x00);
	msg->AddU32(windowTextId);
	msg->AddString(text);
}

void ProtocolGame::sendOutfitWindow()
{
	#define MAX_OUTFITS 50

	NetworkMessage_ptr msg = getOutputBuffer();
	if(!msg)
		return;

	TRACK_MESSAGE(msg);
	msg->AddByte(0xC8);

	Outfit_t currentOutfit = player->getDefaultOutfit();
	Mount* currentMount = Mounts::getInstance()->getMountByID(player->getCurrentMount());
	if(currentMount)
		currentOutfit.lookMount = currentMount->getClientID();

	AddCreatureOutfit(msg, player, currentOutfit);

	const OutfitListType& globalOutfits = Outfits::getInstance()->getOutfits(player->getSex());
	std::list<Outfit> outfits;
	if(player->isAccessPlayer())
	{
		Outfit _outfit;
		_outfit.looktype = 75;
		_outfit.addons = 3;
		outfits.push_back(_outfit);

		for(OutfitListType::const_iterator it = globalOutfits.begin(), end = globalOutfits.end(); it != end; ++it)
		{
			Outfit outfit;
			outfit.looktype = (*it)->looktype;
			outfit.addons = 3;
			outfits.push_back(outfit);
			if(outfits.size() >= MAX_OUTFITS)
				break;
		}
	}
	else if(player->isPremium())
	{
		for(OutfitListType::const_iterator it = globalOutfits.begin(), end = globalOutfits.end(); it != end; ++it)
		{
			Outfit outfit;
			outfit.looktype = (*it)->looktype;
			outfit.addons = player->getOutfitAddons((*it)->looktype);
			outfits.push_back(outfit);
			if(outfits.size() >= MAX_OUTFITS)
				break;
		}
	}
	else
	{
		for(OutfitListType::const_iterator it = globalOutfits.begin(), end = globalOutfits.end(); it != end; ++it)
		{
			if((*it)->premium)
				continue;

			Outfit outfit;
			outfit.looktype = (*it)->looktype;
			outfit.addons = player->getOutfitAddons((*it)->looktype);
			outfits.push_back(outfit);
			if(outfits.size() >= MAX_OUTFITS)
				break;
		}
	}

	msg->AddByte(outfits.size());
	for(std::list<Outfit>::const_iterator it = outfits.begin(), end = outfits.end(); it != end; ++it)
	{
		msg->AddU16((*it).looktype);
		msg->AddString(Outfits::getInstance()->getOutfitName((*it).looktype));
		msg->AddByte((*it).addons);
	}

	MountsList mounts;
	for(MountsList::const_iterator it = Mounts::getInstance()->getFirstMount(),
		end = Mounts::getInstance()->getLastMount(); it != end; ++it)
	{
		if((*it)->isTamed(player))
			mounts.push_back(*it);
	}

	msg->AddByte(mounts.size());
	for(MountsList::const_iterator it = mounts.begin(), end = mounts.end(); it != end; ++it)
	{
		msg->AddU16((*it)->getClientID());
		msg->AddString((*it)->getName());
	}

	player->hasRequestedOutfit(true);
}

void ProtocolGame::sendVIPLogIn(uint32_t guid)
{
	NetworkMessage_ptr msg = getOutputBuffer();
	if(!msg)
		return;

	TRACK_MESSAGE(msg);
	msg->AddByte(0xD3);
	msg->AddU32(guid);
}

void ProtocolGame::sendVIPLogOut(uint32_t guid)
{
	NetworkMessage_ptr msg = getOutputBuffer();
	if(!msg)
		return;

	TRACK_MESSAGE(msg);
	msg->AddByte(0xD4);
	msg->AddU32(guid);
}

void ProtocolGame::sendVIP(uint32_t guid, const std::string& name, bool isOnline)
{
	NetworkMessage_ptr msg = getOutputBuffer();
	if(!msg)
		return;

	TRACK_MESSAGE(msg);
	msg->AddByte(0xD2);
	msg->AddU32(guid);
	msg->AddString(name);
	msg->AddByte(isOnline ? 0x01 : 0x00);
}

void ProtocolGame::sendSpellCooldown(uint8_t spellId, uint32_t time)
{
	if(spellId == 0 || spellId > 160)
		return;

	NetworkMessage_ptr msg = getOutputBuffer();
	if(!msg)
		return;

	TRACK_MESSAGE(msg);
	msg->AddByte(0xA4);
	msg->AddByte(spellId);
	msg->AddU32(time);
}

void ProtocolGame::sendSpellGroupCooldown(SpellGroup_t groupId, uint32_t time)
{
	NetworkMessage_ptr msg = getOutputBuffer();
	if(!msg)
		return;

	TRACK_MESSAGE(msg);
	msg->AddByte(0xA5);
	msg->AddByte(groupId);
	msg->AddU32(time);
}

void ProtocolGame::sendDamageMessage(MessageClasses mclass, const std::string& message, const Position& pos,
	uint32_t primaryDamage/* = 0*/, TextColor_t primaryColor/* = TEXTCOLOR_NONE*/,
	uint32_t secondaryDamage/* = 0*/, TextColor_t secondaryColor/* = TEXTCOLOR_NONE*/)
{
	NetworkMessage_ptr msg = getOutputBuffer();
	if(!msg)
		return;

	TRACK_MESSAGE(msg);
	msg->AddByte(0xB4);
	msg->AddByte(mclass);
	msg->AddPosition(pos);
	msg->AddU32(primaryDamage);
	msg->AddByte(primaryColor);
	msg->AddU32(secondaryDamage);
	msg->AddByte(secondaryColor);
	msg->AddString(message);
}

void ProtocolGame::sendHealMessage(MessageClasses mclass, const std::string& message, const Position& pos, uint32_t heal, TextColor_t color)
{
	NetworkMessage_ptr msg = getOutputBuffer();
	if(!msg)
		return;

	TRACK_MESSAGE(msg);
	msg->AddByte(0xB4);
	msg->AddByte(mclass);
	msg->AddPosition(pos);
	msg->AddU32(heal);
	msg->AddByte(color);
	msg->AddString(message);
}

void ProtocolGame::sendExperienceMessage(MessageClasses mclass, const std::string& message, const Position& pos, uint32_t exp, TextColor_t color)
{
	NetworkMessage_ptr msg = getOutputBuffer();
	if(!msg)
		return;

	TRACK_MESSAGE(msg);
	msg->AddByte(0xB4);
	msg->AddByte(mclass);
	msg->AddPosition(pos);
	msg->AddU32(exp);
	msg->AddByte(color);
	msg->AddString(message);
}

////////////// Add common messages
void ProtocolGame::AddMapDescription(NetworkMessage_ptr msg, const Position& pos)
{
	msg->AddByte(0x64);
	msg->AddPosition(player->getPosition());
	GetMapDescription(pos.x - 8, pos.y - 6, pos.z, 18, 14, msg);
}

void ProtocolGame::AddTextMessage(NetworkMessage_ptr msg, MessageClasses mclass, const std::string& message)
{
	msg->AddByte(0xB4);
	msg->AddByte(mclass);
	msg->AddString(message);
}

void ProtocolGame::AddTextMessageEx(NetworkMessage_ptr msg, MessageClasses mclass, const std::string& message, const Position& pos, uint32_t value, TextColor_t color)
{
	msg->AddByte(0xB4);
	msg->AddByte(mclass);
	msg->AddPosition(pos);
	msg->AddU32(value);
	msg->AddByte(color);
	msg->AddString(message);
}

void ProtocolGame::AddMagicEffect(NetworkMessage_ptr msg,const Position& pos, uint8_t type)
{
	msg->AddByte(0x83);
	msg->AddPosition(pos);
	msg->AddByte(type + 1);
}

void ProtocolGame::AddDistanceShoot(NetworkMessage_ptr msg, const Position& from, const Position& to,
	uint8_t type)
{
	msg->AddByte(0x85);
	msg->AddPosition(from);
	msg->AddPosition(to);
	msg->AddByte(type + 1);
}

void ProtocolGame::AddCreature(NetworkMessage_ptr msg, const Creature* creature, bool known, uint32_t remove)
{
	if(known)
	{
		msg->AddU16(0x62);
		msg->AddU32(creature->getID());
	}
	else
	{
		msg->AddU16(0x61);
		msg->AddU32(remove);
		msg->AddU32(creature->getID());
		msg->AddByte(creature->getType());
		msg->AddString(creature->getName());
	}

	if(creature->isHealthHidden())
		msg->AddByte(0x00);
	else
		msg->AddByte((int32_t)std::ceil(((float)creature->getHealth()) * 100 / std::max(creature->getMaxHealth(), (int32_t)1)));

	msg->AddByte((uint8_t)creature->getDirection());

	if(!creature->isInvisible() && !creature->isInGhostMode())
		AddCreatureOutfit(msg, creature, creature->getCurrentOutfit());
	else
		AddCreatureInvisible(msg, creature);

	LightInfo lightInfo;
	creature->getCreatureLight(lightInfo);
	msg->AddByte(player->isAccessPlayer() ? 0xFF : lightInfo.level);
	msg->AddByte(lightInfo.color);

	msg->AddU16(creature->getStepSpeed());

	msg->AddByte(player->getSkullClient(creature->getPlayer()));
	msg->AddByte(player->getPartyShield(creature->getPlayer()));

	if(!known)
		msg->AddByte(player->getGuildEmblem(creature->getPlayer()));

	msg->AddByte(player->isAccessPlayer() ? 0x00 : 0x01);
}

void ProtocolGame::AddPlayerStats(NetworkMessage_ptr msg)
{
	msg->AddByte(0xA0);

	msg->AddU16(player->getHealth());
	msg->AddU16(player->getPlayerInfo(PLAYERINFO_MAXHEALTH));

	msg->AddU32(uint32_t(player->getFreeCapacity() * 100));
	msg->AddU32(uint32_t(player->getCapacity() * 100));

	msg->AddU64(player->getExperience());

	msg->AddU16(player->getPlayerInfo(PLAYERINFO_LEVEL));
	msg->AddByte(player->getPlayerInfo(PLAYERINFO_LEVELPERCENT));

	msg->AddU16(player->getMana());
	msg->AddU16(player->getPlayerInfo(PLAYERINFO_MAXMANA));

	msg->AddByte(player->getMagicLevel());
	msg->AddByte(player->getBaseMagicLevel());
	msg->AddByte(player->getPlayerInfo(PLAYERINFO_MAGICLEVELPERCENT));

	msg->AddByte(player->getPlayerInfo(PLAYERINFO_SOUL));

	msg->AddU16(0x9D8); // stamina minutes

	msg->AddU16(player->getBaseSpeed());

	Condition* condition = player->getCondition(CONDITION_REGENERATION);
	msg->AddU16(condition ? condition->getTicks() / 1000 : 0x00);

	msg->AddU16(player->getOfflineTrainingTime() / 60 / 1000);
}

void ProtocolGame::AddPlayerSkills(NetworkMessage_ptr msg)
{
	msg->AddByte(0xA1);

	msg->AddByte(player->getSkill(SKILL_FIST, SKILL_LEVEL));
	msg->AddByte(player->getBaseSkill(SKILL_FIST));
	msg->AddByte(player->getSkill(SKILL_FIST, SKILL_PERCENT));

	msg->AddByte(player->getSkill(SKILL_CLUB, SKILL_LEVEL));
	msg->AddByte(player->getBaseSkill(SKILL_CLUB));
	msg->AddByte(player->getSkill(SKILL_CLUB, SKILL_PERCENT));

	msg->AddByte(player->getSkill(SKILL_SWORD, SKILL_LEVEL));
	msg->AddByte(player->getBaseSkill(SKILL_SWORD));
	msg->AddByte(player->getSkill(SKILL_SWORD, SKILL_PERCENT));

	msg->AddByte(player->getSkill(SKILL_AXE, SKILL_LEVEL));
	msg->AddByte(player->getBaseSkill(SKILL_AXE));
	msg->AddByte(player->getSkill(SKILL_AXE, SKILL_PERCENT));

	msg->AddByte(player->getSkill(SKILL_DIST, SKILL_LEVEL));
	msg->AddByte(player->getBaseSkill(SKILL_DIST));
	msg->AddByte(player->getSkill(SKILL_DIST, SKILL_PERCENT));

	msg->AddByte(player->getSkill(SKILL_SHIELD, SKILL_LEVEL));
	msg->AddByte(player->getBaseSkill(SKILL_SHIELD));
	msg->AddByte(player->getSkill(SKILL_SHIELD, SKILL_PERCENT));

	msg->AddByte(player->getSkill(SKILL_FISH, SKILL_LEVEL));
	msg->AddByte(player->getBaseSkill(SKILL_FISH));
	msg->AddByte(player->getSkill(SKILL_FISH, SKILL_PERCENT));
}

void ProtocolGame::AddCreatureSpeak(NetworkMessage_ptr msg, const Creature* creature, SpeakClasses type,
	std::string text, uint16_t channelId, Position* pos/* = NULL*/)
{
	if(!creature)
		return;

	msg->AddByte(0xAA);

	static uint32_t statementId = 0;
	msg->AddU32(++statementId); // statement id

	if(type == SPEAK_CHANNEL_R2)
	{
		msg->AddU16(0x00);
		type = SPEAK_CHANNEL_R1;
	}
	else
		msg->AddString(creature->getName());

	//Add level only for players
	if(const Player* speaker = creature->getPlayer())
	{
		if(speaker->isAccountManager())
			msg->AddU16(0x00);
		else
			msg->AddU16(speaker->getPlayerInfo(PLAYERINFO_LEVEL));
	}
	else
		msg->AddU16(0x00);

	msg->AddByte(type);
	switch(type)
	{
		case SPEAK_SAY:
		case SPEAK_WHISPER:
		case SPEAK_YELL:
		case SPEAK_MONSTER_SAY:
		case SPEAK_MONSTER_YELL:
		case SPEAK_PRIVATE_NP:
		{
			if(pos)
				msg->AddPosition(*pos);
			else
				msg->AddPosition(creature->getPosition());

			break;
		}

		case SPEAK_CHANNEL_Y:
		case SPEAK_CHANNEL_R1:
		case SPEAK_CHANNEL_O:
			msg->AddU16(channelId);
			break;

		default:
			break;
	}
	msg->AddString(text);
}

void ProtocolGame::AddCreatureHealth(NetworkMessage_ptr msg,const Creature* creature)
{
	msg->AddByte(0x8C);
	msg->AddU32(creature->getID());
	if(creature->isHealthHidden())
		msg->AddByte(0x00);
	else
		msg->AddByte((int32_t)std::ceil(((float)creature->getHealth()) * 100 / std::max(creature->getMaxHealth(), (int32_t)1)));
}

void ProtocolGame::AddCreatureInvisible(NetworkMessage_ptr msg, const Creature* creature)
{
	if(player->canSeeInvisibility() && !creature->isInGhostMode())
		AddCreatureOutfit(msg, creature, creature->getCurrentOutfit());
	else
	{
		msg->AddU16(0x00);
		msg->AddU16(0x00);
		msg->AddU16(0x00);
	}
}

void ProtocolGame::AddCreatureOutfit(NetworkMessage_ptr msg, const Creature* creature, const Outfit_t& outfit)
{
	msg->AddU16(outfit.lookType);

	if(outfit.lookType != 0)
	{
		msg->AddByte(outfit.lookHead);
		msg->AddByte(outfit.lookBody);
		msg->AddByte(outfit.lookLegs);
		msg->AddByte(outfit.lookFeet);
		msg->AddByte(outfit.lookAddons);
	}
	else
		msg->AddItemId(outfit.lookTypeEx);

	msg->AddU16(outfit.lookMount);
}

void ProtocolGame::AddWorldLight(NetworkMessage_ptr msg, const LightInfo& lightInfo)
{
	msg->AddByte(0x82);
	msg->AddByte((player->isAccessPlayer() ? 0xFF : lightInfo.level));
	msg->AddByte(lightInfo.color);
}

void ProtocolGame::AddCreatureLight(NetworkMessage_ptr msg, const Creature* creature)
{
	LightInfo lightInfo;
	creature->getCreatureLight(lightInfo);
	msg->AddByte(0x8D);
	msg->AddU32(creature->getID());
	msg->AddByte((player->isAccessPlayer() ? 0xFF : lightInfo.level));
	msg->AddByte(lightInfo.color);
}

//tile
void ProtocolGame::AddTileItem(NetworkMessage_ptr msg, const Position& pos, uint32_t stackpos, const Item* item)
{
	if(stackpos >= 10)
		return;

	msg->AddByte(0x6A);
	msg->AddPosition(pos);
	msg->AddByte(stackpos);
	msg->AddItem(item);
}

void ProtocolGame::AddTileCreature(NetworkMessage_ptr msg, const Position& pos, uint32_t stackpos,
	const Creature* creature)
{
	if(stackpos >= 10)
		return;

	msg->AddByte(0x6A);
	msg->AddPosition(pos);
	msg->AddByte(stackpos);

	bool known;
	uint32_t removedKnown;
	checkCreatureAsKnown(creature->getID(), known, removedKnown);
	AddCreature(msg, creature, known, removedKnown);
}

void ProtocolGame::UpdateTileItem(NetworkMessage_ptr msg, const Position& pos, uint32_t stackpos, const Item* item)
{
	if(stackpos >= 10)
		return;

	msg->AddByte(0x6B);
	msg->AddPosition(pos);
	msg->AddByte(stackpos);
	msg->AddItem(item);
}

void ProtocolGame::RemoveTileItem(NetworkMessage_ptr msg, const Position& pos, uint32_t stackpos)
{
	if(stackpos >= 10)
		return;

	msg->AddByte(0x6C);
	msg->AddPosition(pos);
	msg->AddByte(stackpos);
}

void ProtocolGame::MoveUpCreature(NetworkMessage_ptr msg, const Creature* creature,
	const Position& newPos, const Position& oldPos, uint32_t oldStackPos)
{
	if(creature != player)
		return;

	//floor change up
	msg->AddByte(0xBE);

	//going to surface
	if(newPos.z == 7)
	{
		int32_t skip = -1;
		GetFloorDescription(msg, oldPos.x - 8, oldPos.y - 6, 5, 18, 14, 3, skip); //(floor 7 and 6 already set)
		GetFloorDescription(msg, oldPos.x - 8, oldPos.y - 6, 4, 18, 14, 4, skip);
		GetFloorDescription(msg, oldPos.x - 8, oldPos.y - 6, 3, 18, 14, 5, skip);
		GetFloorDescription(msg, oldPos.x - 8, oldPos.y - 6, 2, 18, 14, 6, skip);
		GetFloorDescription(msg, oldPos.x - 8, oldPos.y - 6, 1, 18, 14, 7, skip);
		GetFloorDescription(msg, oldPos.x - 8, oldPos.y - 6, 0, 18, 14, 8, skip);

		if(skip >= 0)
		{
			msg->AddByte(skip);
			msg->AddByte(0xFF);
		}
	}
	//underground, going one floor up (still underground)
	else if(newPos.z > 7)
	{
		int32_t skip = -1;
		GetFloorDescription(msg, oldPos.x - 8, oldPos.y - 6, oldPos.z - 3, 18, 14, 3, skip);

		if(skip >= 0)
		{
			msg->AddByte(skip);
			msg->AddByte(0xFF);
		}
	}

	//moving up a floor up makes us out of sync
	//west
	msg->AddByte(0x68);
	GetMapDescription(oldPos.x - 8, oldPos.y + 1 - 6, newPos.z, 1, 14, msg);

	//north
	msg->AddByte(0x65);
	GetMapDescription(oldPos.x - 8, oldPos.y - 6, newPos.z, 18, 1, msg);
}

void ProtocolGame::MoveDownCreature(NetworkMessage_ptr msg, const Creature* creature,
	const Position& newPos, const Position& oldPos, uint32_t oldStackPos)
{
	if(creature != player)
		return;

	//floor change down
	msg->AddByte(0xBF);

	//going from surface to underground
	if(newPos.z == 8)
	{
		int32_t skip = -1;

		GetFloorDescription(msg, oldPos.x - 8, oldPos.y - 6, newPos.z, 18, 14, -1, skip);
		GetFloorDescription(msg, oldPos.x - 8, oldPos.y - 6, newPos.z + 1, 18, 14, -2, skip);
		GetFloorDescription(msg, oldPos.x - 8, oldPos.y - 6, newPos.z + 2, 18, 14, -3, skip);

		if(skip >= 0)
		{
			msg->AddByte(skip);
			msg->AddByte(0xFF);
		}
	}
	//going further down
	else if(newPos.z > oldPos.z && newPos.z > 8 && newPos.z < 14)
	{
		int32_t skip = -1;
		GetFloorDescription(msg, oldPos.x - 8, oldPos.y - 6, newPos.z + 2, 18, 14, -3, skip);

		if(skip >= 0)
		{
			msg->AddByte(skip);
			msg->AddByte(0xFF);
		}
	}

	//moving down a floor makes us out of sync
	//east
	msg->AddByte(0x66);
	GetMapDescription(oldPos.x + 9, oldPos.y - 1 - 6, newPos.z, 1, 14, msg);

	//south
	msg->AddByte(0x67);
	GetMapDescription(oldPos.x - 8, oldPos.y + 7, newPos.z, 18, 1, msg);
}

//inventory
void ProtocolGame::AddInventoryItem(NetworkMessage_ptr msg, slots_t slot, const Item* item)
{
	if(!item)
	{
		msg->AddByte(0x79);
		msg->AddByte(slot);
		return;
	}

	msg->AddByte(0x78);
	msg->AddByte(slot);
	msg->AddItem(item);
}

void ProtocolGame::UpdateInventoryItem(NetworkMessage_ptr msg, slots_t slot, const Item* item)
{
	if(!item)
	{
		msg->AddByte(0x79);
		msg->AddByte(slot);
		return;
	}

	msg->AddByte(0x78);
	msg->AddByte(slot);
	msg->AddItem(item);
}

void ProtocolGame::RemoveInventoryItem(NetworkMessage_ptr msg, slots_t slot)
{
	msg->AddByte(0x79);
	msg->AddByte(slot);
}

//containers
void ProtocolGame::AddContainerItem(NetworkMessage_ptr msg, uint8_t cid, const Item* item)
{
	msg->AddByte(0x70);
	msg->AddByte(cid);
	msg->AddItem(item);
}

void ProtocolGame::UpdateContainerItem(NetworkMessage_ptr msg, uint8_t cid, uint8_t slot, const Item* item)
{
	msg->AddByte(0x71);
	msg->AddByte(cid);
	msg->AddByte(slot);
	msg->AddItem(item);
}

void ProtocolGame::RemoveContainerItem(NetworkMessage_ptr msg, uint8_t cid, uint8_t slot)
{
	msg->AddByte(0x72);
	msg->AddByte(cid);
	msg->AddByte(slot);
}

void ProtocolGame::sendChannelMessage(std::string author, std::string text, SpeakClasses type, uint8_t channel)
{
	NetworkMessage_ptr msg = getOutputBuffer();
	if(!msg)
		return;

	TRACK_MESSAGE(msg);
	msg->AddByte(0xAA);
	msg->AddU32(0x00);
	msg->AddString(author);
	msg->AddU16(0x00);
	msg->AddByte(type);
	msg->AddU16(channel);
	msg->AddString(text);
}

void ProtocolGame::AddShopItem(NetworkMessage_ptr msg, const ShopInfo& item)
{
	const ItemType& it = Item::items[item.itemId];
	msg->AddU16(it.clientId);
	if(it.stackable || it.isRune())
		msg->AddByte(item.subType);
	else if(it.isSplash() || it.isFluidContainer())
		msg->AddByte(serverFluidToClient(item.subType));
	else
		msg->AddByte(0x00);

	msg->AddString(item.realName);
	msg->AddU32(uint32_t(it.weight * 100));
	msg->AddU32(item.buyPrice);
	msg->AddU32(item.sellPrice);
}
