R"netmodesvoice(
	local teamMT = {}
	teamMT.__index = teamMT

	network.gameModes = {}
	network.gameMode = {
		create = function(name, callbacks)
			if not validName(name, 64) then error("game mode name must be a non-empty string", 2) end
			if callbacks ~= nil and type(callbacks) ~= "table" then error("game mode callbacks must be a table", 2) end
			local mode = clone(callbacks or {})
			mode.name = name
			network.gameModes[name] = mode
			return mode
		end,
	}
	network.gameModes.lobby = network.gameMode.create("lobby")
	network.gameModes.deathmatch = network.gameMode.create("deathmatch", {
		onPlayerDeath = function(_, _, killer)
			if killer then killer:setData("score", (tonumber(killer.data.score) or 0) + 1, { public = true }) end
		end,
	})
	network.gameModes.teamDeathmatch = network.gameMode.create("team-deathmatch", {
		onPlayerDeath = function(_, _, killer)
			if killer and killer.team then
				local team = killer.manager.teams[killer.team]
				if team then team:setScore(team.score + 1) end
			end
		end,
	})
	network.gameModes["team-deathmatch"] = network.gameModes.teamDeathmatch

	local function teamSnapshot(session)
		local teams = {}
		for name, team in pairs(session.teams) do
			teams[#teams + 1] = { name = name, score = team.score, maxPlayers = team.maxPlayers }
		end
		table.sort(teams, function(a, b) return a.name < b.name end)
		return teams
	end

	function sessionMT:_sendTeamsTo(peer)
		self:_send(peer, { type = "team_snapshot", teams = teamSnapshot(self) }, true)
	end

	function sessionMT:createTeam(name, options)
		if self.role ~= "server" then error("only the server can create teams", 2) end
		if not validName(name, 32) then error("team name must be a non-empty string", 2) end
		if self.teams[name] then error("team already exists: " .. name, 2) end
		options = options or {}
		local maxPlayers = options.maxPlayers or self.maxPlayers
		if not finite(maxPlayers) or maxPlayers < 1 then error("team maxPlayers must be a positive number", 2) end
		local team = setmetatable({
			manager = self,
			name = name,
			score = 0,
			maxPlayers = math.floor(maxPlayers),
			players = {},
		}, teamMT)
		self.teams[name] = team
		self:_broadcast({ type = "team_snapshot", teams = teamSnapshot(self) }, true)
		self:_emit("TeamCreated", team)
		return team
	end

	function teamMT:setScore(score)
		if self.manager.role ~= "server" then error("only the server can change team scores", 2) end
		if not finite(score) then error("team score must be finite", 2) end
		self.score = score
		self.manager:_broadcast({ type = "team_snapshot", teams = teamSnapshot(self.manager) }, true)
		self.manager:_emit("TeamScoreChanged", self, score)
		return self
	end

	function teamMT:getPlayers()
		local result = {}
		for id in pairs(self.players) do
			local player = self.manager:getPlayer(id)
			if player then result[#result + 1] = player end
		end
		return result
	end

	function sessionMT:setPlayerTeam(playerId, teamName)
		if self.role ~= "server" then error("only the server can change player teams", 2) end
		local player = self:getPlayer(playerId)
		if not player then return false end
		local nextTeam = teamName and self.teams[teamName] or nil
		if teamName and not nextTeam then return false end
		if nextTeam and not nextTeam.players[player.id] then
			local count = 0
			for _ in pairs(nextTeam.players) do count = count + 1 end
			if count >= nextTeam.maxPlayers then return false end
		end
		if player.team and self.teams[player.team] then self.teams[player.team].players[player.id] = nil end
		player.team = teamName
		if nextTeam then nextTeam.players[player.id] = true end
		self:_sendPlayerData(player)
		self:_emit("PlayerTeamChanged", player, teamName)
		return true
	end

	function sessionMT:getTeams()
		local result = {}
		for name, team in pairs(self.teams) do
			result[name] = { name = name, score = team.score, maxPlayers = team.maxPlayers }
		end
		return result
	end

	function sessionMT:_receiveTeams(message)
		if type(message.teams) ~= "table" then return false end
		self.teams = {}
		for _, record in ipairs(message.teams) do
			if type(record) == "table" and validName(record.name, 32) then
				self.teams[record.name] = {
					name = record.name,
					score = finite(record.score) and record.score or 0,
					maxPlayers = finite(record.maxPlayers) and record.maxPlayers or 0,
				}
			end
		end
		self:_emit("TeamsChanged", self:getTeams())
		return true
	end

	function sessionMT:setGameMode(mode)
		if self.role ~= "server" then error("only the server can select a game mode", 2) end
		if type(mode) == "string" then mode = network.gameModes and network.gameModes[mode] end
		if type(mode) ~= "table" or not validName(mode.name, 64) then
			error("setGameMode expects a game mode created by love.mgold.gameMode.create", 2)
		end
		if self.state == "playing" then error("cannot change game mode during a match", 2) end
		self.gameMode = mode
		if mode.onInitialize then mode:onInitialize(self) end
		self:_emit("GameModeChanged", mode)
		self:_broadcast({ type = "game_state", state = self.state, mode = mode.name }, true)
		return self
	end

	function sessionMT:spawnPlayer(playerId)
		if self.role ~= "server" then error("only the server can spawn players", 2) end
		local player = self:getPlayer(playerId)
		if not player then return false end
		self:_emit("PlayerSpawn", player)
		return true
	end

	function sessionMT:playerDied(playerId, killerId)
		if self.role ~= "server" then error("only the server can report player deaths", 2) end
		local player = self:getPlayer(playerId)
		local killer = killerId and self:getPlayer(killerId) or nil
		if not player then return false end
		self:_emit("PlayerDeath", player, killer)
		return true
	end

	function sessionMT:startGame()
		if self.role ~= "server" then error("only the server can start a game", 2) end
		if self.state == "playing" then return false end
		self.state = "playing"
		if self.gameMode and self.gameMode.onStart then self.gameMode:onStart(self) end
		self:_emit("GameStart", self.gameMode)
		self:_broadcast({ type = "game_state", state = self.state, mode = self.gameMode and self.gameMode.name }, true)
		return true
	end

	function sessionMT:returnToLobby()
		if self.role ~= "server" then error("only the server can return to the lobby", 2) end
		if self.gameMode and self.gameMode.onEnd then self.gameMode:onEnd(self) end
		self.state = "lobby"
		self:_emit("ReturnToLobby", self.gameMode)
		self:_broadcast({ type = "game_state", state = self.state, mode = self.gameMode and self.gameMode.name }, true)
		return true
	end

	function sessionMT:sendVoice(frame, channel)
		if self.role ~= "client" then error("sendVoice is a client operation", 2) end
		if not self.peer or not self.voice.enabled then return false end
		if self.voice.pushToTalk and not self.pushToTalkActive then return false end
		if self.voice.encode then frame = self.voice.encode(frame) end
		if type(frame) ~= "string" or #frame == 0 or #frame > self.voice.maxPacketBytes then return false end
		channel = channel or "global"
		if not validName(channel, 32) then return false end
		self:_send(self.peer, { type = "voice_request", data = frame, channel = channel }, false)
		return true
	end

	function sessionMT:setPushToTalk(active)
		if type(active) ~= "boolean" then error("push-to-talk state must be boolean", 2) end
		self.pushToTalkActive = active
		return self
	end

	function sessionMT:setPlayerVoiceVolume(playerId, volume)
		if self.role ~= "client" then error("setPlayerVoiceVolume is a client operation", 2) end
		if not validId(playerId) or not finite(volume) then error("invalid voice volume", 2) end
		self.voiceVolumes[playerId] = math.max(0, math.min(1, volume))
		return self
	end

	function sessionMT:getPlayerVoiceVolume(playerId)
		if self.role ~= "client" then error("getPlayerVoiceVolume is a client operation", 2) end
		return self.voiceVolumes[playerId] or 1
	end

	function sessionMT:setVoiceCodec(encode, decode)
		if encode ~= nil and type(encode) ~= "function" then error("voice encoder must be a function or nil", 2) end
		if decode ~= nil and type(decode) ~= "function" then error("voice decoder must be a function or nil", 2) end
		self.voice.encode, self.voice.decode = encode, decode
		return self
	end

	function sessionMT:mutePlayer(playerId)
		if self.role ~= "client" then error("mutePlayer is a client operation", 2) end
		if not validId(playerId) then return false end
		self.mutedPlayers[playerId] = true
		return true
	end

	function sessionMT:unmutePlayer(playerId)
		if self.role ~= "client" then error("unmutePlayer is a client operation", 2) end
		if not validId(playerId) then return false end
		self.mutedPlayers[playerId] = nil
		return true
	end

	function sessionMT:muteAll()
		if self.role ~= "client" then error("muteAll is a client operation", 2) end
		self.muteAllVoice = true
		return self
	end

	function sessionMT:unmuteAll()
		if self.role ~= "client" then error("unmuteAll is a client operation", 2) end
		self.muteAllVoice = false
		return self
	end

	function sessionMT:_handleVoice(peer, message)
		local sender = self.players[peer]
		if not sender or not self.voice.enabled or type(message.data) ~= "string" or
			#message.data == 0 or #message.data > self.voice.maxPacketBytes or
			not validName(message.channel or "global", 32) or not self:_allowRate(peer, "voice", self.voice.maxPacketsPerSecond, 1) then
			return true
		end
		local channel = message.channel or "global"
		if channel == "team" and not self.voice.team then return true end
		if channel == "global" and not self.voice.global then return true end
		if channel ~= "team" and channel ~= "global" and
			(type(self.voice.channels) ~= "table" or self.voice.channels[channel] ~= true) then return true end
		self:_emit("VoicePacketReceived", sender, message.data, channel)
		for _, listener in ipairs(self:getPlayers()) do
			if listener.id ~= sender.id then
				local allowed
				local dx = sender.state.position.x - listener.state.position.x
				local dy = sender.state.position.y - listener.state.position.y
				local dz = sender.state.position.z - listener.state.position.z
				local distance = math.sqrt(dx * dx + dy * dy + dz * dz)
				if self.voice.canHear then
					allowed = self.voice.canHear(sender, listener, channel, self, distance) == true
				elseif channel == "team" then
					allowed = sender.team ~= nil and sender.team == listener.team
				else
					allowed = self.voice.global
				end
				if allowed and self.voice.maxDistance > 0 then allowed = distance <= self.voice.maxDistance end
				if allowed then
					self:_send(listener.peer, {
						type = "voice_packet",
						senderId = sender.id,
						data = message.data,
						channel = channel,
					}, false)
				end
			end
		end
		return true
	end

	function sessionMT:_receiveVoiceMessage(message)
		if message.type ~= "voice_packet" then return false end
		if not self.voice.enabled or self.muteAllVoice or self.mutedPlayers[message.senderId] then return true end
		if type(message.data) ~= "string" or #message.data > self.voice.maxPacketBytes then return true end
		local data = message.data
		if self.voice.decode then data = self.voice.decode(data) end
		local volume = self:getPlayerVoiceVolume(message.senderId)
		if self.voice.onPacket then self.voice.onPacket(message.senderId, data, message.channel or "global", volume) end
		self:_emit("VoicePacket", message.senderId, data, message.channel or "global", volume)
		return true
	end

)netmodesvoice"
