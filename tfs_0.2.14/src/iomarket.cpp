//////////////////////////////////////////////////////////////////////
// OpenTibia - an opensource roleplaying game
//////////////////////////////////////////////////////////////////////
// IOMarket
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

#include "iomarket.h"
#include "iologindata.h"
#include "configmanager.h"

extern ConfigManager g_config;

MarketOfferList IOMarket::getActiveOffers(MarketAction_t action, uint16_t itemId)
{
	MarketOfferList offerList;

	DBQuery query;
	query << "SELECT `id`, `player_id`, `amount`, `price`, `created`, `anonymous` FROM `market_offers` WHERE `sale` = " << action << " AND `itemtype` = " << itemId << ";";

	Database* db = Database::getInstance();
	DBResult* result;
	if(!(result = db->storeQuery(query.str())))
		return offerList;

	const int32_t marketOfferDuration = g_config.getNumber(ConfigManager::MARKET_OFFER_DURATION);
	do
	{
		MarketOffer offer;
		offer.amount = result->getDataInt("amount");
		offer.price = result->getDataInt("price");
		offer.timestamp = result->getDataInt("created") + marketOfferDuration;
		offer.counter = result->getDataInt("id") & 0xFFFF;
		if(result->getDataInt("anonymous") == 0)
		{
			IOLoginData::getInstance()->getNameByGuid(result->getDataInt("player_id"), offer.playerName);
			if(offer.playerName.empty())
				offer.playerName = "Anonymous";
		}
		else
			offer.playerName = "Anonymous";

		offerList.push_back(offer);
	}
	while(result->next());
	db->freeResult(result);
	return offerList;
}

MarketOfferList IOMarket::getOwnOffers(MarketAction_t action, uint32_t playerId)
{
	MarketOfferList offerList;

	const int32_t marketOfferDuration = g_config.getNumber(ConfigManager::MARKET_OFFER_DURATION);

	DBQuery query;
	query << "SELECT `id`, `amount`, `price`, `created`, `anonymous`, `itemtype` FROM `market_offers` WHERE `player_id` = " << playerId << " AND `sale` = " << action << ";";

	Database* db = Database::getInstance();
	DBResult* result;
	if(!(result = db->storeQuery(query.str())))
		return offerList;

	do
	{
		MarketOffer offer;
		offer.amount = result->getDataInt("amount");
		offer.price = result->getDataInt("price");
		offer.timestamp = result->getDataInt("created") + marketOfferDuration;
		offer.counter = result->getDataInt("id") & 0xFFFF;
		offer.itemId = result->getDataInt("itemtype");

		offerList.push_back(offer);
	}
	while(result->next());
	db->freeResult(result);
	return offerList;
}

HistoryMarketOfferList IOMarket::getOwnHistory(MarketAction_t action, uint32_t playerId)
{
	HistoryMarketOfferList offerList;

	DBQuery query;
	query << "SELECT `id`, `itemtype`, `amount`, `price`, `expires_at`, `state` FROM `market_history` WHERE `player_id` = " << playerId << " AND `sale` = " << action << ";";

	Database* db = Database::getInstance();
	DBResult* result;
	if(!(result = db->storeQuery(query.str())))
		return offerList;

	do
	{
		HistoryMarketOffer offer;
		offer.itemId = result->getDataInt("itemtype");
		offer.amount = result->getDataInt("amount");
		offer.price = result->getDataInt("price");
		offer.timestamp = result->getDataInt("expires_at");

		MarketOfferState_t offerState = (MarketOfferState_t)result->getDataInt("state");
		if(offerState == OFFERSTATE_ACCEPTEDEX)
			offerState = OFFERSTATE_ACCEPTED;

		offer.state = offerState;

		offerList.push_back(offer);
	}
	while(result->next());
	db->freeResult(result);
	return offerList;
}

ExpiredMarketOfferList IOMarket::getExpiredOffers(MarketAction_t action)
{
	ExpiredMarketOfferList offerList;

	const time_t lastExpireDate = time(NULL) - g_config.getNumber(ConfigManager::MARKET_OFFER_DURATION);

	DBQuery query;
	query << "SELECT `id`, `amount`, `price`, `itemtype`, `player_id` FROM `market_offers` WHERE `sale` = " << action << " AND `created` <= " << lastExpireDate << ";";

	Database* db = Database::getInstance();
	DBResult* result;
	if(!(result = db->storeQuery(query.str())))
		return offerList;

	do
	{
		ExpiredMarketOffer offer;
		offer.id = result->getDataInt("id");
		offer.amount = result->getDataInt("amount");
		offer.price = result->getDataInt("price");
		offer.itemId = result->getDataInt("itemtype");
		offer.playerId = result->getDataInt("player_id");

		offerList.push_back(offer);
	}
	while(result->next());
	db->freeResult(result);
	return offerList;
}

int32_t IOMarket::getPlayerOfferCount(uint32_t playerId)
{
	int32_t count = -1;
	Database* db = Database::getInstance();
	DBResult* result;

	DBQuery query;
	query << "SELECT COUNT(*) AS `count` FROM `market_offers` WHERE `player_id` = " << playerId << ";";
	if(!(result = db->storeQuery(query.str())))
		return count;

	count = result->getDataInt("count");
	db->freeResult(result);
	return count;
}

MarketOfferEx IOMarket::getOfferById(uint32_t id)
{
	MarketOfferEx offer;
	DBQuery query;
	query << "SELECT `id`, `sale`, `itemtype`, `amount`, `created`, `price`, `player_id`, `anonymous` FROM `market_offers` WHERE `id` = " << id << ";";
	Database* db = Database::getInstance();
	DBResult* result;
	if((result = db->storeQuery(query.str())))
	{
		offer.type = (MarketAction_t)result->getDataInt("sale");
		offer.amount = result->getDataInt("amount");
		offer.counter = result->getDataInt("id") & 0xFFFF;
		offer.timestamp = result->getDataInt("created");
		offer.price = result->getDataInt("price");
		offer.itemId = result->getDataInt("itemtype");

		int32_t playerId = result->getDataInt("player_id");
		offer.playerId = playerId;
		if(result->getDataInt("anonymous") == 0)
		{
			IOLoginData::getInstance()->getNameByGuid(playerId, offer.playerName);
			if(offer.playerName.empty())
				offer.playerName = "Anonymous";
		}
		else
			offer.playerName = "Anonymous";

		db->freeResult(result);
	}
	return offer;
}

uint32_t IOMarket::getOfferIdByCounter(uint32_t timestamp, uint16_t counter)
{
	const int32_t created = timestamp - g_config.getNumber(ConfigManager::MARKET_OFFER_DURATION);

	DBQuery query;
	query << "SELECT `id` FROM `market_offers` WHERE `created` = " << created << " AND (`id` & 65535) = " << counter << " LIMIT 1;";
	Database* db = Database::getInstance();
	DBResult* result;
	if((result = db->storeQuery(query.str())))
	{
		uint32_t offerId = result->getDataInt("id");
		db->freeResult(result);
		return offerId;
	}
	return 0;
}

void IOMarket::createOffer(uint32_t playerId, MarketAction_t action, uint32_t itemId, uint16_t amount, uint32_t price, bool anonymous)
{
	DBQuery query;
	query << "INSERT INTO `market_offers` (`player_id`, `sale`, `itemtype`, `amount`, `price`, `created`, `anonymous`) VALUES (" << playerId << ", " << action << ", " << itemId << ", " << amount << ", " << price << ", " << time(NULL) << ", " << anonymous << ");";
	Database::getInstance()->executeQuery(query.str());
}

void IOMarket::acceptOffer(uint32_t offerId, uint16_t amount)
{
	DBQuery query;
	query << "UPDATE `market_offers` SET `amount` = `amount` - " << amount << " WHERE `id` = " << offerId << ";";
	Database::getInstance()->executeQuery(query.str());
}

void IOMarket::deleteOffer(uint32_t offerId)
{
	DBQuery query;
	query << "DELETE FROM `market_offers` WHERE `id` = " << offerId << ";";
	Database::getInstance()->executeQuery(query.str());
}

void IOMarket::appendHistory(uint32_t playerId, MarketAction_t type, uint16_t itemId, uint16_t amount, uint32_t price, time_t timestamp, MarketOfferState_t state)
{
	DBQuery query;
	query << "INSERT INTO `market_history` (`player_id`, `sale`, `itemtype`, `amount`, `price`, `expires_at`, `inserted`, `state`) VALUES "
		<< "(" << playerId << ", " << type << ", " << itemId << ", " << amount << ", " << price << ", "
		<< timestamp << ", " << time(NULL) << ", " << state << ");";
	Database::getInstance()->executeQuery(query.str());
}

void IOMarket::moveOfferToHistory(uint32_t offerId, MarketOfferState_t state)
{
	const int32_t marketOfferDuration = g_config.getNumber(ConfigManager::MARKET_OFFER_DURATION);

	Database* db = Database::getInstance();

	DBQuery query;
	DBResult* result;
	query << "SELECT `player_id`, `sale`, `itemtype`, `amount`, `price`, `created` FROM `market_offers` WHERE `id` = " << offerId << ";";
	if(!(result = db->storeQuery(query.str())))
		return;

	query.str("");
	query << "DELETE FROM `market_offers` WHERE `id` = " << offerId << ";";
	if(!db->executeQuery(query.str()))
	{
		db->freeResult(result);
		return;
	}

	appendHistory(result->getDataInt("player_id"), (MarketAction_t)result->getDataInt("sale"), result->getDataInt("itemtype"), result->getDataInt("amount"), result->getDataInt("price"), result->getDataInt("created") + marketOfferDuration, state);
	db->freeResult(result);
}

void IOMarket::clearOldHistory()
{
	const time_t lastExpireDate = time(NULL) - g_config.getNumber(ConfigManager::MARKET_OFFER_DURATION);
	DBQuery query;
	query << "DELETE FROM `market_history` WHERE `inserted` <= " << lastExpireDate << ";";
	Database::getInstance()->executeQuery(query.str());
}

void IOMarket::updateStatistics()
{
	Database* db = Database::getInstance();

	DBQuery query;
	query << "SELECT `sale` AS `sale`, `itemtype` AS `itemtype`, COUNT(`price`) AS `num`, MIN(`price`) AS `min`, MAX(`price`) AS `max`, SUM(`price`) AS `sum` FROM `market_history` WHERE `state` = " << OFFERSTATE_ACCEPTED << " GROUP BY `itemtype`, `sale`;";

	DBResult* result;
	if(!(result = db->storeQuery(query.str())))
		return;

	do
	{
		MarketStatistics* statistics;
		if(result->getDataInt("sale") == MARKETACTION_BUY)
			statistics = &purchaseStatistics[result->getDataInt("itemtype")];
		else
			statistics = &saleStatistics[result->getDataInt("itemtype")];

		statistics->numTransactions = result->getDataInt("num");
		statistics->lowestPrice = result->getDataInt("min");
		statistics->totalPrice = result->getDataLong("sum");
		statistics->highestPrice = result->getDataInt("max");
	}
	while(result->next());
	db->freeResult(result);
}

MarketStatistics* IOMarket::getPurchaseStatistics(uint16_t itemId)
{
	std::map<uint16_t, MarketStatistics>::iterator it = purchaseStatistics.find(itemId);
	if(it == purchaseStatistics.end())
		return NULL;

	return &it->second;
}

MarketStatistics* IOMarket::getSaleStatistics(uint16_t itemId)
{
	std::map<uint16_t, MarketStatistics>::iterator it = saleStatistics.find(itemId);
	if(it == saleStatistics.end())
		return NULL;

	return &it->second;
}
