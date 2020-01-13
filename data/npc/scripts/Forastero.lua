--[[
    Created by: Tu Padre Bala
    Created date: 1/12/2020
    Modified by: -
    Modified date: -
    Name: [NPC] Forastero
    Desc: Script para manejar la interaccion del NPC para dar misiones cada 24 horas a los players.
    Las misiones seran random de una lista
--]]

local keywordHandler = KeywordHandler:new()
local npcHandler = NpcHandler:new(keywordHandler)
NpcSystem.parseParameters(npcHandler)

function onCreatureAppear(cid)                npcHandler:onCreatureAppear(cid)             end
function onCreatureDisappear(cid)             npcHandler:onCreatureDisappear(cid)         end
function onCreatureSay(cid, type, msg)         npcHandler:onCreatureSay(cid, type, msg)     end
function onThink()                             npcHandler:onThink() end

local forastero_variantes = {
    {lugar_x=4055 , lugar_desc = "las planicies al sur del monte de Patricios Guerreros.", item = 6542, item_desc = "huevos rojos"},
    {lugar_x=820 , lugar_desc = "un oasis al oeste del desierto de los speedy gonzales", item = 2144, item_desc = "black pearls"},
    {lugar_x=1431 , lugar_desc = "un lugar al centro de la selva la cabrona", item = 5911, item_desc = "red piece of cloth"}
}

function creatureSayCallback(cid, type, msg)
if(not npcHandler:isFocused(cid)) then
	return false
end

local talkUser = NPCHANDLER_CONVBEHAVIOR == CONVERSATION_PRIVATE and 0 or cid
local exhaustTime = 24 * 60 * 60
--local exhaustTime = 30 -- For testing
local time_storage = 18845
local state_storage = 18846
local pos_storage = 18847
local hours_last = 3
local exp_boost_item = 1.25

if (msgcontains(msg, 'termine')) then

    local exp_boost_actual = round(getPlayerExpMultiplier(cid),4)

    local state_value = getPlayerStorageValue(cid,state_storage)

    if state_value <= 0 then
        npcHandler:say('No te he dado ninguna reto {rapidin}', cid)
        npcHandler:resetNpc()
        return TRUE
    end

    local item_id = forastero_variantes[state_value].item
    local expected_pos = forastero_variantes[state_value].lugar_x
    
    local pos_action = getPlayerStorageValue(cid,pos_storage)

    if pos_action ~= expected_pos then
        npcHandler:say('No usaste el crystal que te pedi en el reto.', cid)
        npcHandler:resetNpc()
        return TRUE
    end

    local player_trae = getPlayerItemCount(cid, item_id)

    if player_trae < 50 then
        npcHandler:say('No traes los items que te pedi.', cid)
        npcHandler:resetNpc()
        return TRUE
    end

    if exp_boost_actual >= exp_boost_item then
        npcHandler:say("Ya tienes un boost igual o mayor, regresa cuando se te acabe.", cid)
        npcHandler:resetNpc()
        return TRUE
    else
        setPlayerStorageValue(cid, 19256, os.time()+(hours_last*60*60))
        setPlayerExpMultiplier(cid, exp_boost_item)
        doSendMagicEffect(getCreaturePosition(cid), CONST_ME_FIREWORK_RED)

        doPlayerRemoveItem(cid,item_id,50)
        setPlayerStorageValue(cid,time_storage,os.time()+(exhaustTime))
        setPlayerStorageValue(cid,state_storage,-1)
        npcHandler:say("Aqui tienes un boost de 25% por 3 horas", cid)
        npcHandler:resetNpc()
        return TRUE
    end
end


if(msgcontains(msg, 'rapidin')) then
    local time_storage = 18845
    local state_storage = 18846
    local pos_storage = 18847


    local time_value = getPlayerStorageValue(cid,time_storage)
    local state_value = getPlayerStorageValue(cid,state_storage)
    --local variante_value = getPlayerStorageValue(cid,variante_storage)
    

    if ( time_value>os.time() and time_value ~= -1) then
        npcHandler:say('Sorry, Aun no han pasado 24 desde que terminaste el reto pasado. Bye', cid)
        npcHandler:resetNpc()
        return true
    end

    if state_value > 0 then
        local item_desc = forastero_variantes[state_value].item_desc
        local lugar = forastero_variantes[state_value].lugar_desc
        npcHandler:say('Ya te pedi un reto. Te la recuerdo por si te apendejas: pedi que me traigas 50 ' .. item_desc ..' y le des use a un pilar de crystal raro en '.. lugar.. '. Cuando acabes, ven y dime {termine}', cid)
        npcHandler:resetNpc()
        return false
    end

    -- TODO: Logic for random missions from list
    
    setPlayerStorageValue(cid,state_storage,1)
    setPlayerStorageValue(cid,pos_storage,-1)

    local item_desc = forastero_variantes[1].item_desc
    local lugar = forastero_variantes[1].lugar_desc

    npcHandler:say('Ok, Ocupo 2 cosas, que me traigas 50 ' .. item_desc ..' y le des use a un pilar de crystal en '.. lugar.. '. Cuando acabes, ven y dime {termine}', cid)

    --npcHandler:resetNpc()
end
return true
end

npcHandler:setCallback(CALLBACK_MESSAGE_DEFAULT, creatureSayCallback)
npcHandler:addModule(FocusModule:new())
