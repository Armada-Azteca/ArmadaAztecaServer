--[[
    Created by: MrTrala (GM Grinch)
    Created date: 5/08/2020
    Modified by: -
    Modified date: -
    Name: !irbooth
	Desc: Script para uso de characters de GMs, para ir al booth concedido por los GODs.
	Ejemplo:
	!irbooth -- Empezara un timer de 3 segundos para despues ser transportado a la posicion del booth.
--]]

local storageDelTiempo = 12345 -- Storage donde se guardara el tiempo a poder usar el comando.
local storageDelBooth = 12346 -- Storage donde esta guardado el booth que tiene acceso el character del GM.
local storageDelCD = 12347 -- Storage donde sera guardado el CD del comando.
local tiempoCD = 5 * 60 -- Tiempo para poder volver a usar el comando, para evitar GMs corruptos que intenten spamear el OT.
local numeroDeBooths = { -- Numeros de booths disponibles, esta tabla junto con la de !gmbooth pueden ser convinadas en global.lua
    [1] = {pos = {x = 2108, y = 2654, z = 7}}, -- El numero de orden en la tabla es el numero que se usara para ver cual booth te fue otorgado.
    [2] = {pos = {x = 166, y = 55, z = 7}},
    [3] = {pos = {x = 166, y = 55, z = 7}},
    [4] = {pos = {x = 166, y = 55, z = 7}},
    [5] = {pos = {x = 166, y = 55, z = 7}}
}

function onSay(cid, words, param)
    local checarCD = getPlayerStorageValue(cid, storageDelCD) - os.time()
    if checarCD > 0 then
        return warnPlayer(cid, 'Necesitas esperar 5 minutos para volver a usar este comando.') and false
    end

    local tiempo = getPlayerStorageValue(cid, storageDelTiempo) - os.time()
    if tiempo <= 0 then
        setPlayerStorageValue(cid, storageDelCD, (os.time() + tiempoCD))
        return doPlayerSendTextMessage(cid, MESSAGE_STATUS_CONSOLE_BLUE, 'Pelas! No tienes tiempo restante para ir al booth!') and false
    end

    local pos = getPlayerPosition(cid)
    local tile = getTilePzInfo(pos)
    local boothStorage = getPlayerStorageValue(cid, storageDelBooth)
    local booth = numeroDeBooths[boothStorage]

    if not tile then
        setPlayerStorageValue(cid, storageDelCD, (os.time() + tiempoCD))
        return doPlayerSendTextMessage(cid, MESSAGE_STATUS_CONSOLE_BLUE, 'Necesitas estar en PZ para irte a tu booth!') and false
    end

    if not booth then
        setPlayerStorageValue(cid, storageDelCD, (os.time() + tiempoCD))
        return doPlayerSendTextMessage(cid, MESSAGE_STATUS_CONSOLE_BLUE, 'Hubo un error al recuperar a que booth vas! (Reportaselo a Lady Morrigan o Bala!)') and false
    end

    doSendMagicEffect(pos, 10)
    doTeleportThing(cid, booth.pos, true)
    doSendMagicEffect(booth.pos, 23)
    setPlayerStorageValue(cid, storageDelCD, (os.time() + tiempoCD))

	doPlayerSendTextMessage(cid, MESSAGE_STATUS_CONSOLE_BLUE, "Has sido transportado al GM Booth!")
	return true
end