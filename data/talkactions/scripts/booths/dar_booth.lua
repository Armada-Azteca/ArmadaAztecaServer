--[[
    Created by: MrTrala (GM Grinch)
    Created date: 5/08/2020
    Modified by: -
    Modified date: -
    Name: !gmbooth
	Desc: Script para uso de GODs, para dar acceso X tiempo a un personaje a algun GM Booth.
	Ejemplo:
	!gmbooth "12, Pon Vips, 1 -- Se le otorgara el acceso al comando !irgmbooth al personaje Pon Vips por 12 horas y cuando lo use ira al GM booth numero 1.
--]]

local storageDelTiempo = 12345 -- Storage donde se guardara el tiempo a usar.
local storageDelBooth = 12346 -- Storage donde se guardara a que booth tiene acceso el character del GM.
local accesoParaUsar = 2 -- Acceso que tiene que tener la persona para poder usar el script (2 = GM)  -- Cambiar a GOD en Armada Azteca.
local numeroDeBooths = {1, 2, 3, 4, 5} -- Numeros de booths disponibles, ver tabla da posiciones en comando !irgmbooth -- Puede ser movido a global, junto con positions.

function onSay(cid, words, param)
	local group = getPlayerGroupId(cid)
	if group < accesoParaUsar then
		return false
	end

    if param == nil or param == '' then
        return doPlayerSendTextMessage(cid, MESSAGE_STATUS_CONSOLE_BLUE, 'Error en syntax: !gmbooth "HORAS, NOMBRE, NUMERO DE BOOTH') and false
    end

	local param = string.explode(param, ",")

	if not param[1] or not param[2] or not param[3] or string.match(param[1], "[%a%p]") or string.match(param[3], "[%a%p]") then
		return doPlayerSendTextMessage(cid, MESSAGE_STATUS_CONSOLE_BLUE, 'Error en syntax: !gmbooth "HORAS, NOMBRE, NUMERO DE BOOTH') and false
	end

	local tiempo = tonumber(param[1]) * 60 * 60
	local player = getPlayerByName(param[2])
	local booth = tonumber(param[3])

	if not player then
		return doPlayerSendTextMessage(cid, MESSAGE_STATUS_CONSOLE_BLUE, "El jugador "..param[2].." no existe o no esta conectado.") and false
	elseif not table.find(numeroDeBooths, booth) then
		return doPlayerSendTextMessage(cid, MESSAGE_STATUS_CONSOLE_BLUE, "No existe o no esta configurado el booth numero "..booth..".") and false
	end

	local nombre = getPlayerName(cid)

	setPlayerStorageValue(player, storageDelTiempo, (os.time() + tiempo))
	setPlayerStorageValue(player, storageDelBooth, booth)

	doPlayerSendTextMessage(player, MESSAGE_STATUS_CONSOLE_BLUE, "Te han sido otorgadas "..param[1].." hora/s por "..nombre.." al GM Booth numero "..param[3]..", para ir (en PZ) usa !irbooth.")
	doPlayerSendTextMessage(cid, MESSAGE_STATUS_CONSOLE_BLUE, "Otorgaste acceso por "..param[1].." hora/s al jugador "..param[2].." al GM Booth numero "..param[3]..".")
	return true
end