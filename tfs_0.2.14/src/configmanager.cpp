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

#include "definitions.h"
#include "configmanager.h"
#include "tools.h"

#include <iostream>
#include <stdexcept>

ConfigManager::ConfigManager()
{
	m_isLoaded = false;
}

ConfigManager::~ConfigManager()
{
	//
}

bool ConfigManager::loadFile(const std::string& _filename)
{
	lua_State* L = lua_open();
	if(!L)
		throw std::runtime_error("Failed to allocate memory");

	if(luaL_dofile(L, _filename.c_str()))
	{
		std::cout << "[Error - ConfigManager::loadFile] With file = " << _filename << ", " << lua_tostring(L, -1) << std::endl;
		lua_close(L);
		return false;
	}

	//parse config
	if(!m_isLoaded) //info that must be loaded one time (unless we reset the modules involved)
	{
		m_confBoolean[SERVERSAVE_ENABLED] = booleanString(getGlobalString(L, "serverSaveEnabled", "yes"));
		m_confBoolean[SAVE_GLOBAL_STORAGE] = booleanString(getGlobalString(L, "saveGlobalStorage", "no"));
		m_confBoolean[INGAME_GUILD_SYSTEM] = booleanString(getGlobalString(L, "ingameGuildSystem", "yes"));
		m_confBoolean[BIND_ONLY_GLOBAL_ADDRESS] = booleanString(getGlobalString(L, "bindOnlyGlobalAddress", "no"));
		m_confBoolean[OPTIMIZE_DATABASE] = booleanString(getGlobalString(L, "startupDatabaseOptimization", "yes"));

		m_confString[CONFIG_FILE] = _filename;
		m_confString[IP] = getGlobalString(L, "ip", "127.0.0.1");
		m_confString[MAP_NAME] = getGlobalString(L, "mapName", "forgotten");
		m_confString[MAP_AUTHOR] = getGlobalString(L, "mapAuthor", "Unknown");
		m_confString[HOUSE_RENT_PERIOD] = getGlobalString(L, "houseRentPeriod", "monthly");
		m_confString[MYSQL_HOST] = getGlobalString(L, "mysqlHost", "localhost");
		m_confString[MYSQL_USER] = getGlobalString(L, "mysqlUser", "root");
		m_confString[MYSQL_PASS] = getGlobalString(L, "mysqlPass", "");
		m_confString[MYSQL_DB] = getGlobalString(L, "mysqlDatabase", "theforgottenserver");
		m_confString[SQLITE_DB] = getGlobalString(L, "sqliteDatabase");
		m_confString[PASSWORDTYPE] = getGlobalString(L, "passwordType", "plain");
		#ifdef MULTI_SQL_DRIVERS
		m_confString[SQL_TYPE] = getGlobalString(L, "sqlType", "sqlite");
		#endif

		m_confInteger[SQL_PORT] = getGlobalNumber(L, "mysqlPort", 3306);
		m_confInteger[PASSWORD_TYPE] = PASSWORD_TYPE_PLAIN;
		m_confInteger[SERVERSAVE_H] = getGlobalNumber(L, "serverSaveHour", 3);
		m_confInteger[ADMIN_PORT] = getGlobalNumber(L, "adminProtocolPort", 7171);
		m_confInteger[GAME_PORT] = getGlobalNumber(L, "gameProtocolPort", 7172);
		m_confInteger[LOGIN_PORT] = getGlobalNumber(L, "loginProtocolPort", 7171);
		m_confInteger[STATUS_PORT] = getGlobalNumber(L, "statusProtocolPort", 7171);

		m_confInteger[MARKET_OFFER_DURATION] = getGlobalNumber(L, "marketOfferDuration",  30 * 24 * 60 * 60);
	}

	m_confBoolean[FREE_MEMORY_AT_SHUTDOWN] = booleanString(getGlobalString(L, "freeMemoryAtShutdown", "no"));
	m_confBoolean[ACCOUNT_MANAGER] = booleanString(getGlobalString(L, "accountManager", "yes"));
	m_confBoolean[ON_OR_OFF_CHARLIST] = booleanString(getGlobalString(L, "displayOnOrOffAtCharlist", "no"));
	m_confBoolean[ALLOW_CHANGEOUTFIT] = booleanString(getGlobalString(L, "allowChangeOutfit", "yes"));
	m_confBoolean[ONE_PLAYER_ON_ACCOUNT] = booleanString(getGlobalString(L, "onePlayerOnlinePerAccount", "yes"));
	m_confBoolean[CANNOT_ATTACK_SAME_LOOKFEET] = booleanString(getGlobalString(L, "noDamageToSameLookfeet", "no"));
	m_confBoolean[AIMBOT_HOTKEY_ENABLED] = booleanString(getGlobalString(L, "hotkeyAimbotEnabled", "yes"));
	m_confBoolean[START_CHOOSEVOC] = booleanString(getGlobalString(L, "newPlayerChooseVoc", "no"));
	m_confBoolean[SHOW_GAMEMASTERS_ONLINE] = booleanString(getGlobalString(L, "displayGamemastersWithOnlineCommand", "no"));
	m_confBoolean[REMOVE_AMMO] = booleanString(getGlobalString(L, "removeAmmoWhenUsingDistanceWeapon", "yes"));
	m_confBoolean[REMOVE_RUNE_CHARGES] = booleanString(getGlobalString(L, "removeChargesFromRunes", "yes"));
	m_confBoolean[REMOVE_WEAPON_CHARGES] = booleanString(getGlobalString(L, "removeChargesFromWeapons", "yes"));
	m_confBoolean[RANDOMIZE_TILES] = booleanString(getGlobalString(L, "randomizeTiles", "yes"));
	m_confBoolean[EXPERIENCE_FROM_PLAYERS] = booleanString(getGlobalString(L, "experienceByKillingPlayers", "no"));
	m_confBoolean[SHUTDOWN_AT_SERVERSAVE] = booleanString(getGlobalString(L, "shutdownAtServerSave", "no"));
	m_confBoolean[CLEAN_MAP_AT_SERVERSAVE] = booleanString(getGlobalString(L, "cleanMapAtServerSave", "yes"));
	m_confBoolean[FREE_PREMIUM] = booleanString(getGlobalString(L, "freePremium", "no"));
	m_confBoolean[ADMIN_LOGS_ENABLED] = booleanString(getGlobalString(L, "adminLogsEnabled", "no"));
	m_confBoolean[BROADCAST_BANISHMENTS] = booleanString(getGlobalString(L, "broadcastBanishments", "yes"));
	m_confBoolean[GENERATE_ACCOUNT_NUMBER] = booleanString(getGlobalString(L, "generateAccountNumber", "yes"));
	m_confBoolean[REPLACE_KICK_ON_LOGIN] = booleanString(getGlobalString(L, "replaceKickOnLogin", "yes"));
	m_confBoolean[OLD_CONDITION_ACCURACY] = booleanString(getGlobalString(L, "oldConditionAccuracy", "no"));
	m_confBoolean[ALLOW_CLONES] = booleanString(getGlobalString(L, "allowClones", "no"));
	m_confBoolean[MARKET_ENABLED] = booleanString(getGlobalString(L, "marketEnabled", "yes"));
	m_confBoolean[MARKET_PREMIUM] = booleanString(getGlobalString(L, "premiumToCreateMarketOffer", "yes"));

	m_confString[DEFAULT_PRIORITY] = getGlobalString(L, "defaultPriority", "high");
	m_confString[MAP_STORAGE_TYPE] = getGlobalString(L, "mapStorageType", "relational");
	m_confString[LOGIN_MSG] = getGlobalString(L, "loginMessage", "Welcome to the Forgotten Server!");
	m_confString[SERVER_NAME] = getGlobalString(L, "serverName");
	m_confString[OWNER_NAME] = getGlobalString(L, "ownerName");
	m_confString[OWNER_EMAIL] = getGlobalString(L, "ownerEmail");
	m_confString[URL] = getGlobalString(L, "url");
	m_confString[LOCATION] = getGlobalString(L, "location");
	m_confString[MOTD] = getGlobalString(L, "motd");
	m_confString[WORLD_TYPE] = getGlobalString(L, "worldType", "pvp");

	m_confInteger[LOGIN_TRIES] = getGlobalNumber(L, "loginTries", 3);
	m_confInteger[RETRY_TIMEOUT] = getGlobalNumber(L, "retryTimeout", 30 * 1000);
	m_confInteger[LOGIN_TIMEOUT] = getGlobalNumber(L, "loginTimeout", 5 * 1000);
	m_confInteger[MAX_PLAYERS] = getGlobalNumber(L, "maxPlayers");
	m_confInteger[PZ_LOCKED] = getGlobalNumber(L, "pzLocked", 0);
	m_confInteger[DEFAULT_DESPAWNRANGE] = getGlobalNumber(L, "deSpawnRange", 2);
	m_confInteger[DEFAULT_DESPAWNRADIUS] = getGlobalNumber(L, "deSpawnRadius", 50);
	m_confInteger[RATE_EXPERIENCE] = getGlobalNumber(L, "rateExp", 1);
	m_confInteger[RATE_SKILL] = getGlobalNumber(L, "rateSkill", 1);
	m_confInteger[RATE_LOOT] = getGlobalNumber(L, "rateLoot", 1);
	m_confInteger[RATE_MAGIC] = getGlobalNumber(L, "rateMagic", 1);
	m_confInteger[RATE_SPAWN] = getGlobalNumber(L, "rateSpawn", 1);
	m_confInteger[SPAWNPOS_X] = getGlobalNumber(L, "newPlayerSpawnPosX", 100);
	m_confInteger[SPAWNPOS_Y] = getGlobalNumber(L, "newPlayerSpawnPosY", 100);
	m_confInteger[SPAWNPOS_Z] = getGlobalNumber(L, "newPlayerSpawnPosZ", 7);
	m_confInteger[SPAWNTOWN_ID] = getGlobalNumber(L, "newPlayerTownId", 1);
	m_confInteger[START_LEVEL] = getGlobalNumber(L, "newPlayerLevel", 1);
	m_confInteger[START_MAGICLEVEL] = getGlobalNumber(L, "newPlayerMagicLevel", 0);
	m_confInteger[HOUSE_PRICE] = getGlobalNumber(L, "housePriceEachSQM", 1000);
	m_confInteger[KILLS_TO_RED] = getGlobalNumber(L, "killsToRedSkull", 3);
	m_confInteger[KILLS_TO_BLACK] = getGlobalNumber(L, "killsToBlackSkull", 6);
	m_confInteger[KILLS_TO_BAN] = getGlobalNumber(L, "killsToBan", 5);
	m_confInteger[BAN_DAYS] = getGlobalNumber(L, "banDays", 7);
	m_confInteger[FINAL_BAN_DAYS] = getGlobalNumber(L, "finalBanDays", 30);
	m_confInteger[HIGHSCORES_TOP] = getGlobalNumber(L, "highscoreDisplayPlayers", 10);
	m_confInteger[HIGHSCORES_UPDATETIME] = getGlobalNumber(L, "updateHighscoresAfterMinutes", 60);
	m_confInteger[ACTIONS_DELAY_INTERVAL] = getGlobalNumber(L, "timeBetweenActions", 200);
	m_confInteger[EX_ACTIONS_DELAY_INTERVAL] = getGlobalNumber(L, "timeBetweenExActions", 1000);
	m_confInteger[MAX_MESSAGEBUFFER] = getGlobalNumber(L, "maxMessageBuffer", 4);
	m_confInteger[CRITICAL_HIT_CHANCE] = getGlobalNumber(L, "criticalHitChance", 5);
	m_confInteger[KICK_AFTER_MINUTES] = getGlobalNumber(L, "kickIdlePlayerAfterMinutes", 15);
	m_confInteger[PROTECTION_LEVEL] = getGlobalNumber(L, "protectionLevel", 1);
	m_confInteger[DEATH_LOSE_PERCENT] = getGlobalNumber(L, "deathLosePercent", -1);
	m_confInteger[STATUSQUERY_TIMEOUT] = getGlobalNumber(L, "statusTimeout", 5 * 60 * 1000);
	m_confInteger[FRAG_TIME] = getGlobalNumber(L, "timeToDecreaseFrags", 24 * 60 * 60 * 1000);
	m_confInteger[WHITE_SKULL_TIME] = getGlobalNumber(L, "whiteSkullTime", 15 * 60 * 1000);
	m_confInteger[AUTO_SAVE_EACH_MINUTES] = getGlobalNumber(L, "autoSaveEachMinutes", 0);
	m_confInteger[STAIRHOP_DELAY] = getGlobalNumber(L, "stairJumpExhaustion", 2000);
	m_confInteger[LEVEL_TO_CREATE_GUILD] = getGlobalNumber(L, "levelToCreateGuild", 8);
	m_confInteger[MIN_GUILD_NAME] = getGlobalNumber(L, "minGuildNameLength", 4);
	m_confInteger[MAX_GUILD_NAME] = getGlobalNumber(L, "maxGuildNameLength", 20);
	m_confInteger[CHECK_EXPIRED_MARKET_OFFERS_EACH_MINUTES] = getGlobalNumber(L, "checkExpiredMarketOffersEachMinutes", 60);
	m_confInteger[MAX_MARKET_OFFERS_AT_A_TIME_PER_PLAYER] = getGlobalNumber(L, "maxMarketOffersAtATimePerPlayer", 100);

	m_isLoaded = true;
	lua_close(L);
	return true;
}

bool ConfigManager::reload()
{
	if(!m_isLoaded)
		return false;

	return loadFile(m_confString[CONFIG_FILE]);
}

const std::string& ConfigManager::getString(uint32_t _what) const
{
	if(m_isLoaded && _what < LAST_STRING_CONFIG)
		return m_confString[_what];
	else
	{
		std::cout << "Warning: [ConfigManager::getString] " << _what << std::endl;
		return m_confString[DUMMY_STR];
	}
}

int32_t ConfigManager::getNumber(uint32_t _what) const
{
	if(m_isLoaded && _what < LAST_INTEGER_CONFIG)
		return m_confInteger[_what];
	else
	{
		std::cout << "Warning: [ConfigManager::getNumber] " << _what << std::endl;
		return 0;
	}
}

bool ConfigManager::getBoolean(uint32_t _what) const
{
	if(m_isLoaded && _what < LAST_BOOLEAN_CONFIG)
		return m_confBoolean[_what];
	else
	{
		std::cout << "Warning: [ConfigManager::getBoolean] " << _what << std::endl;
		return false;
	}
}

bool ConfigManager::setNumber(uint32_t _what, int32_t _value)
{
	if(m_isLoaded && _what < LAST_INTEGER_CONFIG)
	{
		m_confInteger[_what] = _value;
		return true;
	}
	else
	{
		std::cout << "Warning: [ConfigManager::setNumber] " << _what << std::endl;
		return false;
	}
}

std::string ConfigManager::getGlobalString(lua_State* _L, const std::string& _identifier, const std::string& _default)
{
	lua_getglobal(_L, _identifier.c_str());

	if(!lua_isstring(_L, -1))
		return _default;

	int32_t len = (int32_t)lua_strlen(_L, -1);
	std::string ret(lua_tostring(_L, -1), len);
	lua_pop(_L,1);

	return ret;
}

int32_t ConfigManager::getGlobalNumber(lua_State* _L, const std::string& _identifier, const int32_t _default)
{
	lua_getglobal(_L, _identifier.c_str());

	if(!lua_isnumber(_L, -1))
		return _default;

	int32_t val = (int32_t)lua_tonumber(_L, -1);
	lua_pop(_L,1);

	return val;
}

std::string ConfigManager::getGlobalStringField (lua_State* _L, const std::string& _identifier, const int32_t _key, const std::string& _default) {
	lua_getglobal(_L, _identifier.c_str());

	lua_pushnumber(_L, _key);
	lua_gettable(_L, -2);  /* get table[key] */
	if(!lua_isstring(_L, -1))
		return _default;
	std::string result = lua_tostring(_L, -1);
	lua_pop(_L, 2);  /* remove number and key*/
	return result;
}
