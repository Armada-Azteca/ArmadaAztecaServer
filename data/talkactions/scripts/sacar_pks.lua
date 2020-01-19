function onSay(cid, words, param)
	--{x=326,y=357,z=7}
	--{x=326,y=364,z=7}
	--{x=348,y=357,z=7}
	--{x=348,y=364,z=7}
	local group = getPlayerGroupId(cid)
	if (group<2) then
		return FALSE
	end
	doSummonCreature("Player Killer", {x=326,y=357,z=7})
	doSummonCreature("Player Killer", {x=326,y=364,z=7})
	doSummonCreature("Player Killer", {x=348,y=357,z=7})
	doSummonCreature("Player Killer", {x=348,y=364,z=7})
	return TRUE
end
