--[[
    Created by: Tu Padre Bala
    Created date: 1/12/2020
    Modified by: -
    Modified date: -
    Name: !achingarasumadre
    Desc: Script para desaparecer items enfrente del char, solo tiene efecto en los items de la lista
--]]

local lista_basura = {2604, 2105, 12799, 12800}
local storageValue = 112604
local exhaustTime = 5

function onSay(cid, words, param)

    local value=getPlayerStorageValue(cid,storageValue)
    if ( value>os.time() and value ~= -1) then
        setPlayerStorageValue(cid,storageValue,os.time()+(exhaustTime))
		warnPlayer(cid, "Sorry, Tienes que esperar 5 segundos para usar el comando de nuevo.")
        return FALSE
    end
    
    setPlayerStorageValue(cid,storageValue,os.time()+(exhaustTime))

    local position = getPlayerPosition(cid)
    local direction = getCreatureLookDirection(cid)
    local new_pos = {x=position.x,y=position.y,z=position.z}
    
    if direction == NORTH then
        new_pos.y = new_pos.y - 1
    elseif direction == SOUTH then
        new_pos.y = new_pos.y - 1
    elseif direction == EAST then
        new_pos.x = new_pos.x + 1
    else
        new_pos.x = new_pos.x - 1
    end
    new_pos.stackpos = 255
    local object = getThingfromPos(new_pos)
    
    
    if( isInArray(lista_basura, object.itemid) == TRUE and object.uid > 65535 and isCreature(object.uid) == FALSE and isMovable(object.uid) == TRUE and object.actionid == 0) then
		doRemoveItem(object.uid)
		doSendMagicEffect(new_pos, CONST_ME_BLOCKHIT)
        return TRUE
    else
        warnPlayer(cid, "No se puede.")
        return TRUE
	end 
    return TRUE
end
