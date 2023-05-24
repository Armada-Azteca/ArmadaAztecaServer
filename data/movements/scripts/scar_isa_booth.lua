function sacalo_isabooth(cid)
    setPlayerStorageValue(cid,20176,0)
    doSendMagicEffect(getPlayerPosition(cid),CONST_ME_TELEPORT)
    doTeleportThing(cid,{x=160,y=54,z=7},TRUE)
    doSendMagicEffect({x=160,y=54,z=7},CONST_ME_TELEPORT)
    doCreatureSay(cid, 'Saquese!', TALKTYPE_ORANGE_1)
end

function restale_isabooth(cid)
    local time_initial = getPlayerStorageValue(cid,20177)
    local seconds_initial = getPlayerStorageValue(cid,20174)
    if time_initial <=0 then
        time_initial = os.time()
    end

    local seconds_consumed = os.time() - time_initial
    setPlayerStorageValue(cid,20174,seconds_initial-seconds_consumed)
end

function onStepIn(cid, item, position, fromPosition)
    if isPlayer(cid)==FALSE and getPlayerGroupId(cid) > 1 then
        return TRUE
    end

    if item.actionid==20173 then
        local time_initial = getPlayerStorageValue(cid,20177)
        if time_initial <=0 then
            time_initial = os.time()
        end
        local usados = os.time() - time_initial
        if usados >= getPlayerStorageValue(cid,20174) then
            setPlayerStorageValue(cid,20174,0)
            sacalo_isabooth(cid)
        end
    elseif item.actionid==20177 then
        local status = getPlayerStorageValue(cid,20176)
        if status < 0 then 
            status = 0
        end
        if status == 1 then
            restale_isabooth(cid)
        else
            local le_quedan = getPlayerStorageValue(cid,20174)
            if le_quedan > 0 then
                setPlayerStorageValue(cid,20177,os.time())
                doPlayerSendTextMessage(cid,MESSAGE_STATUS_CONSOLE_ORANGE,'Pasale!!')
                setPlayerStorageValue(cid,20176,1)
            else
                doPlayerSendTextMessage(cid,MESSAGE_STATUS_CONSOLE_ORANGE,'No te quedan minutos')
                sacalo_isabooth(cid)
            end
        end
    elseif item.actionid==20178 then
        setPlayerStorageValue(cid,20176,0)
        restale_isabooth(cid)
    end
    return TRUE
end
