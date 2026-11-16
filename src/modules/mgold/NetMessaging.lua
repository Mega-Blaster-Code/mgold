R"netmessaging(
	local function chatConfig(session)
		local defaults = { enabled = true, global = true, team = true, private = true, maxLength = 256 }
		for key, value in pairs(session.options.chat or {}) do defaults[key] = value end
		return defaults
	end

	local function sendCommandResult(session, peer, success, message)
		if peer then
			session:_send(peer, { type = "command_result", success = success == true, message = tostring(message or "") }, true)
		end
	end

	local function tokenize(line)
		local args, current, quoted, escaped = {}, {}, false, false
		for i = 1, #line do
			local char = line:sub(i, i)
			if escaped then
				current[#current + 1] = char
				escaped = false
			elseif char == "\\" and quoted then
				escaped = true
			elseif char == '"' then
				quoted = not quoted
			elseif char:match("%s") and not quoted then
				if #current > 0 then args[#args + 1] = table.concat(current); current = {} end
			else
				current[#current + 1] = char
			end
		end
		if quoted or escaped then return nil end
		if #current > 0 then args[#args + 1] = table.concat(current) end
		return args
	end

	function sessionMT:registerCommand(name, callback, options)
		if self.role ~= "server" then error("only the server can register commands", 2) end
		if not validName(name, 48) or not name:match("^[%w_%-]+$") then
			error("command names may contain only letters, digits, underscore, and hyphen", 2)
		end
		if type(callback) ~= "function" then error("command callback must be a function", 2) end
		self.commands[name:lower()] = { callback = callback, options = clone(options or {}) }
		return self
	end

	function sessionMT:_executeCommand(line, player, remote)
		if type(line) ~= "string" or #line == 0 or #line > self.maxCommandLength then
			return false, "invalid command"
		end
		local args = tokenize(line)
		if not args or #args == 0 then return false, "invalid command syntax" end
		local name = args[1]:lower()
		table.remove(args, 1)
		local entry = self.commands[name]
		if not entry then
			if player then self:_emit("InvalidCommand", player, name) end
			return false, "unknown command"
		end
		local options = entry.options
		if remote and options.serverOnly then return false, "command is server-only" end
		if remote and options.admin and not (player.data.admin == true or player.data.rank == "admin") then
			return false, "permission denied"
		end
		if remote and options.permission and not options.permission(player, self) then
			return false, "permission denied"
		end
		local ok, result = pcall(entry.callback, player, args, self)
		if not ok then
			self:_emit("CommandError", player, name, result)
			return false, "command failed"
		end
		return true, result == nil and "command executed" or tostring(result)
	end

	function sessionMT:runCommand(line)
		if self.role == "client" then
			if not self.peer or not self.ready then return false, "not connected" end
			if type(line) ~= "string" or #line > self.maxCommandLength then return false, "invalid command" end
			self:_send(self.peer, { type = "command_request", line = line }, true)
			return true
		end
		return self:_executeCommand(line, nil, false)
	end

	function sessionMT:_installDefaultCommands()
		self:registerCommand("status", function(_, _, server)
			local lines = { "Players: " .. tostring(server.playerCount) .. "/" .. tostring(server.maxPlayers) }
			for _, player in ipairs(server:getPlayers()) do
				lines[#lines + 1] = tostring(player.id) .. " " .. tostring(player.data.name) .. " (" .. tostring(player:getPing()) .. " ms)"
			end
			return table.concat(lines, "\n")
		end)
		self:registerCommand("say", function(_, args, server)
			server:sendMessage(table.concat(args, " "))
			return "message sent"
		end)
		self:registerCommand("kick", function(_, args, server)
			if not args[1] then return "usage: kick <player id>" end
			local target = server:getPlayer(tonumber(args[1]))
			if not target then return "player not found" end
			server:removePlayer(target.id, "kicked")
			return "player kicked"
		end, { admin = true })
	end

	function sessionMT:sendMessage(text, targetId)
		if self.role ~= "server" then error("only the server can send system chat messages", 2) end
		if type(text) ~= "string" or #text > self.chat.maxLength then error("invalid system chat message", 2) end
		local message = { type = "chat_message", channel = "system", senderId = 0, text = text, time = now() }
		local target = targetId and self:getPlayer(targetId)
		if targetId and not target then return false end
		if target then self:_send(target.peer, message, true)
		else self:_broadcast(message, true) end
		return true
	end

	local function deliverChat(session, player, channel, text, target)
		local message = {
			type = "chat_message",
			channel = channel,
			senderId = player.id,
			senderName = player.data.name,
			targetId = target and target.id or nil,
			text = text,
			time = now(),
		}
		if channel == "private" then
			session:_send(player.peer, message, true)
			session:_send(target.peer, message, true)
		elseif channel == "team" then
			for _, recipient in ipairs(session:getPlayers()) do
				if recipient.team == player.team then session:_send(recipient.peer, message, true) end
			end
		else
			session:_broadcast(message, true)
		end
		session:_emit("ChatMessage", player, channel, text, target)
	end

	function sessionMT:_handleChat(peer, message)
		local player = self.players[peer]
		local channel, text = message.channel, message.text
		if not player or not self.chat.enabled or not self.chat[channel] or
			type(text) ~= "string" or #text == 0 or #text > self.chat.maxLength or
			not self:_allowRate(peer, "chat", 6, 5) then
			self:_emit("InvalidChat", player, message)
			return true
		end
		local target
		if channel == "private" then
			target = self:getPlayer(message.targetId)
			if not target then self:_emit("InvalidChatTarget", player, message.targetId); return true end
		elseif channel == "team" and not player.team then
			self:_emit("InvalidChatTarget", player, "no team")
			return true
		end
		if self.chat.filter then
			text = self.chat.filter(text, player, channel, target)
			if type(text) ~= "string" or #text == 0 or #text > self.chat.maxLength then
				self:_emit("InvalidChat", player, message)
				return true
			end
		end
		deliverChat(self, player, channel, text, target)
		return true
	end

	function sessionMT:chat(text)
		if self.role ~= "client" then error("chat is a client operation", 2) end
		if type(text) ~= "string" or #text == 0 or #text > self.chat.maxLength then return false end
		self:_send(self.peer, { type = "chat_request", channel = "global", text = text }, true)
		return true
	end

	function sessionMT:teamChat(text)
		if self.role ~= "client" then error("teamChat is a client operation", 2) end
		if type(text) ~= "string" or #text == 0 or #text > self.chat.maxLength then return false end
		self:_send(self.peer, { type = "chat_request", channel = "team", text = text }, true)
		return true
	end

	function sessionMT:privateChat(targetId, text)
		if self.role ~= "client" then error("privateChat is a client operation", 2) end
		if not validId(targetId) or type(text) ~= "string" or #text == 0 or #text > self.chat.maxLength then return false end
		self:_send(self.peer, { type = "chat_request", channel = "private", targetId = targetId, text = text }, true)
		return true
	end

	function sessionMT:registerRPC(name, callback, options)
		if not validName(name, 64) then error("RPC name must be a non-empty string", 2) end
		if type(callback) ~= "function" then error("RPC callback must be a function", 2) end
		if self.role == "server" then
			self.serverRPCs[name] = { callback = callback, options = clone(options or {}) }
		else
			self.clientRPCs[name] = callback
		end
		return self
	end

	function sessionMT:registerInput(name, callback, options)
		if self.role ~= "server" then error("only the server can register input handlers", 2) end
		if not validName(name, 64) then error("input name must be a non-empty string", 2) end
		if type(callback) ~= "function" then error("input callback must be a function", 2) end
		self.inputHandlers[name] = { callback = callback, options = clone(options or {}) }
		return self
	end

	function sessionMT:sendInput(name, payload)
		if self.role ~= "client" then error("sendInput is a client operation", 2) end
		if not self.peer or not self.ready or not validName(name, 64) then return false end
		local safePayload, dataError = networkDataCopy(payload)
		if dataError then error(dataError, 2) end
		local packet = { type = "input_request", name = name, payload = safePayload }
		pack(packet)
		self:_send(self.peer, packet, false)
		return true
	end

	function sessionMT:sendRPC(name, payload)
		if self.role ~= "client" then error("sendRPC is a client operation", 2) end
		if not self.peer or not validName(name, 64) then return false end
		local safePayload, dataError = networkDataCopy(payload)
		if dataError then error(dataError, 2) end
		local packet = { type = "rpc_request", name = name, payload = safePayload }
		pack(packet)
		self:_send(self.peer, packet, true)
		return true
	end

	function sessionMT:_handleRPC(peer, message)
		local player = self.players[peer]
		local entry = self.serverRPCs[message.name]
		if not player or not entry or not self:_allowRate(peer, "rpc", 20, 1) then
			self:_emit("InvalidRPC", player, message.name)
			return true
		end
		local payload, dataError = networkDataCopy(message.payload)
		if dataError then
			self:_emit("ProtocolViolation", peer, dataError)
			return true
		end
		local allowed = true
		if entry.options.permission then allowed = entry.options.permission(player, payload, self) == true end
		if not allowed then
			self:_emit("UnauthorizedRPC", player, message.name)
			return true
		end
		entry.callback(player, payload, self)
		return true
	end

	function sessionMT:_handleInput(peer, message)
		local player = self.players[peer]
		local entry = self.inputHandlers[message.name]
		if not player or not entry or not self:_allowRate(peer, "input", 60, 1) then
			self:_emit("InvalidInput", player, message.name)
			return true
		end
		local payload, dataError = networkDataCopy(message.payload)
		if dataError then
			self:_emit("ProtocolViolation", peer, dataError)
			return true
		end
		local allowed = true
		if entry.options.permission then allowed = entry.options.permission(player, payload, self) == true end
		if allowed then entry.callback(player, payload, self)
		else self:_emit("UnauthorizedInput", player, message.name) end
		return true
	end

	function entityMT:getNetworked(key)
		return clone(self.networkData[key])
	end

	function sessionMT:_entityDataForPeer(entity, peer)
		local data = {}
		for key, value in pairs(entity.networkData) do
			if entity.networkVisibility[key] == true or entity.owner == peer then
				data[key] = clone(value)
			end
		end
		return data
	end

	function entityMT:setNetworked(key, value, options)
		if self.manager.role ~= "server" then error("only the server can change networked entity data", 2) end
		if not validName(key, 128) then error("entity data key must be a non-empty string", 2) end
		options = options or {}
		local safeValue, dataError = networkDataCopy(value)
		if dataError then error(dataError, 2) end
		pack({ type = "entity_data", id = self.id, key = key, value = safeValue })
		local wasPublic = self.networkVisibility[key] == true
		self.networkData[key] = safeValue
		self.networkVisibility[key] = options.public == true
		for peer in pairs(self.manager.peers) do
			if options.public == true or self.owner == peer then
				self.manager:_send(peer, {
					type = "entity_data",
					id = self.id,
					key = key,
					value = clone(safeValue),
				}, true)
			elseif wasPublic then
				self.manager:_send(peer, { type = "entity_data_remove", id = self.id, key = key }, true)
			end
		end
		return self
	end

	function sessionMT:_receiveServerFeatures(peer, message)
		if message.type == "chat_request" then return self:_handleChat(peer, message) end
		if message.type == "input_request" then return self:_handleInput(peer, message) end
		if message.type == "command_request" then
			local player = self.players[peer]
			if not player or not self:_allowRate(peer, "command", 5, 5) then return true end
			local success, result = self:_executeCommand(message.line, player, true)
			sendCommandResult(self, peer, success, result)
			return true
		end
		if message.type == "rpc_request" then return self:_handleRPC(peer, message) end
		if message.type == "voice_request" then return self:_handleVoice(peer, message) end
		if message.type == "vote_request" then return self:_startVoteFromPeer(peer, message) end
		if message.type == "vote_cast" then return self:_castVoteFromPeer(peer, message) end
		return false
	end

	function sessionMT:_receiveClientFeatures(message)
		if message.type == "chat_message" then
			self:_emit("ChatMessageReceived", clone(message))
		elseif message.type == "command_result" then
			self:_emit("CommandResult", message.success == true, tostring(message.message or ""))
		elseif message.type == "rpc" then
			local callback = self.clientRPCs[message.name]
			if callback then callback(clone(message.payload), self) end
		elseif message.type == "entity_data" then
			local entity = self.entities[message.id]
			if entity and validName(message.key, 128) then
				entity.networkData[message.key] = clone(message.value)
				self:_emit("EntityDataChanged", entity, message.key, clone(message.value))
			end
		elseif message.type == "entity_data_remove" then
			local entity = self.entities[message.id]
			if entity and validName(message.key, 128) then
				entity.networkData[message.key] = nil
				self:_emit("EntityDataChanged", entity, message.key, nil)
			end
		elseif message.type == "team_snapshot" then
			self:_receiveTeams(message)
		elseif message.type == "game_state" then
			self.state = message.state
			self:_emit("GameStateChanged", self.state, message.mode)
		elseif message.type == "vote_state" or message.type == "vote_result" then
			self:_receiveVoteMessage(message)
		else
			return self:_receiveVoiceMessage(message)
		end
		return true
	end

)netmessaging"
