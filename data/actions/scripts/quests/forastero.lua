--[[
    Created by: Tu Padre Bala
    Created date: 1/12/2020
    Modified by: -
    Modified date: -
    Name: [Action] Pilar de mision de Forastero
    Desc: Script para el pilar que estara ubicado en varias zonas random del mapa, y es lo que forastero pide en sus misiones.
    Es el mismo actionid en todos los lugares, cuando el player le da use, solo guardamos el valor X de la pos para identificar a cual le picaron
    
--]]

function onUse(cid, item, fromPosition, itemEx, toPosition)

    if item.actionid == 18846 then
        doSendMagicEffect(getPlayerPosition(cid), CONST_ME_FIREWORK_RED)
        setPlayerStorageValue(cid, 18847,fromPosition.x )
	    return TRUE
    end
    return TRUE
    
end
