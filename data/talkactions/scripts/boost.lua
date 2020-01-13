--[[
    Created by: Tu Padre Bala
    Created date: 1/12/2020
    Modified by: -
    Modified date: -
    Name: !booster
    Desc: Script para saber si el char tiene algun exp boost activo y por cuanto tiempo
--]]

local storageValue = 12168
local exhaustTime = 5 * 60


function onSay(cid, words, param)
    local value=getPlayerStorageValue(cid,storageValue)

    if ( value>os.time() and value ~= -1) then
        setPlayerStorageValue(cid,storageValue,os.time()+(exhaustTime))
		warnPlayer(cid, "Sorry, Tienes que esperar 5 minutos para usar el comando de nuevo.")
        return FALSE
    end
	
	local name = getPlayerName(cid)
    local exp_boost_actual = round(getPlayerExpMultiplier(cid),4)
    local base_boost = 1
    
    if exp_boost_actual > base_boost then
        local boost_ends = getPlayerStorageValue(cid, 19256)
        local minutes = round((boost_ends - os.time())/60,0)
        doCreatureSay(cid,'Tienes boost a '.. (exp_boost_actual%1)*100 .. '% por '..minutes..' minutos', TALKTYPE_ORANGE_1)
        setPlayerStorageValue(cid,storageValue,os.time()+(exhaustTime))
    else
        doCreatureSay(cid,'No tienes exp booster activo', TALKTYPE_ORANGE_1)
        setPlayerStorageValue(cid,storageValue,os.time()+(exhaustTime))
    end
    
	return FALSE
end