--[[
    Created by: MrTrala (GM Grinch)
    Created date: 5/08/2020
    Modified by: -
    Modified date: -
    Name: !sacargm
	Desc: Script para uso de GODs, para sacar (y quitar el tiempo restante) al character de un GM del GM booth.
	Ejemplo:
	!sacargm "Pon Vips -- Se transportara el character a su templo y se le quitara el acceso completo a !irbooth
--]]

local storageDelTiempo = 12345 -- Storage donde se guardara el tiempo a usar.
local storageDelBooth = 12346 -- Storage donde se guardara a que booth tiene acceso el character del GM.
local accesoParaUsar = 2 -- Acceso que tiene que tener la persona para poder usar el script (2 = GM) -- Cambiar a GOD en Armada Azteca.

function onSay(cid, words, param)
	local group = getPlayerGroupId(cid)
	if group < accesoParaUsar then
		return false
	end

    if param == nil or param == '' then
        return doPlayerSendTextMessage(cid, MESSAGE_STATUS_CONSOLE_BLUE, 'Error en syntax: !sacargm "NOMBRE') and false
    end

    local player = getPlayerByName(param)
    if not player then
        return doPlayerSendTextMessage(cid, MESSAGE_STATUS_CONSOLE_BLUE, "El jugador "..param.." no existe o no esta conectado.") and false
    end

    local pos = getPlayerPosition(player)
    local town = getPlayerTown(player)
    local temple = getTownTemplePosition(town)
    local nombre = getPlayerName(cid)

    doSendMagicEffect(pos, 23)
    doTeleportThing(player, temple, true)
    doSendMagicEffect(temple, 10)
	setPlayerStorageValue(player, storageDelTiempo, -1)
    setPlayerStorageValue(player, storageDelBooth, -1)

	doPlayerSendTextMessage(cid, MESSAGE_STATUS_CONSOLE_BLUE, "Has sacado a "..param.." del booth y su tiempo (si quedaba) a sido removido!")
	doPlayerSendTextMessage(player, MESSAGE_STATUS_CONSOLE_BLUE, ""..nombre.." te ha sacado del booth!")
	return true
end