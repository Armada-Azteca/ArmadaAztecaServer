function onStepIn(cid, item, position, fromPosition)
    if item.actionid == 18494 and isPlayer(cid) then
        local mission_config = special_booths_missions[1]
        local booth_index = 1
        local global_lastplayer_storage = 1002000 + 60 + booth_index

        if getGlobalStorageValue(global_lastplayer_storage) == getPlayerGUID(cid) then
            doTeleportThing(cid, {x=160,y=54,z=7}, TRUE)
            doCreatureSay(cid, 'No puedes tener el booth 2 veces consecutivas', TALKTYPE_ORANGE_1)
            return TRUE
        end

        doRemoveItem(item.uid)
        setGlobalStorageValue(mission_config.global_id, getPlayerGUID(cid))
        doSendMagicEffect(position, CONST_ME_FIREWORK_RED)
        doSendMagicEffect(position, CONST_ME_TUTORIALARROW)
        doCreatureSay(cid, 'Corre a la entrada del booth!', TALKTYPE_ORANGE_1)
        broadcastMessage( getPlayerName(cid) .. ' encontro el portal de la mission de discord', MESSAGE_EVENT_ADVANCE)
	end
	return TRUE
end

function onAddItem(moveitem, tileitem, position)
    doRemoveItem(moveitem.uid, -1)
    doSendMagicEffect(position, CONST_ME_TELEPORT)
	return TRUE
end