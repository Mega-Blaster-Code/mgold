R"netmoderation(
	local function copyOptions(defaults, options)
		for key, value in pairs(options or {}) do defaults[key] = value end
		return defaults
	end

	function sessionMT:_initializeFramework()
		self.maxPacketBytes = self.options.maxPacketBytes or 65536
		if not finite(self.maxPacketBytes) or self.maxPacketBytes < 1024 then
			error("maxPacketBytes must be at least 1024", 2)
		end
		if self.options.chat ~= nil and type(self.options.chat) ~= "table" then error("chat options must be a table", 2) end
		if self.options.voice ~= nil and type(self.options.voice) ~= "table" then error("voice options must be a table", 2) end
		if self.options.voteKick ~= nil and type(self.options.voteKick) ~= "table" then error("voteKick options must be a table", 2) end
		self.chat = chatConfig(self)
		if not finite(self.chat.maxLength) or self.chat.maxLength < 1 or self.chat.maxLength > self.maxPacketBytes then
			error("chat.maxLength must be positive and within maxPacketBytes", 2)
		end
		if self.chat.filter ~= nil and type(self.chat.filter) ~= "function" then error("chat.filter must be a function", 2) end
		self.voice = copyOptions({
			enabled = false,
			global = true,
			team = false,
			maxPacketBytes = 4096,
			maxPacketsPerSecond = 50,
			maxDistance = 0,
		}, self.options.voice)
		if not finite(self.voice.maxPacketBytes) or self.voice.maxPacketBytes < 1 or
			self.voice.maxPacketBytes > self.maxPacketBytes or
			not finite(self.voice.maxPacketsPerSecond) or self.voice.maxPacketsPerSecond < 1 or
			not finite(self.voice.maxDistance) or self.voice.maxDistance < 0 then
			error("voice packet size and rate limits are invalid", 2)
		end
		for _, callbackName in ipairs({ "encode", "decode", "canHear", "onPacket" }) do
			if self.voice[callbackName] ~= nil and type(self.voice[callbackName]) ~= "function" then
				error("voice." .. callbackName .. " must be a function", 2)
			end
		end
		if self.voice.channels ~= nil and type(self.voice.channels) ~= "table" then
			error("voice.channels must be a table", 2)
		end
		self.mutedPlayers = {}
		self.muteAllVoice = false
		self.voiceVolumes = {}
		self.pushToTalkActive = false
		self.voteKick = copyOptions({
			enabled = false,
			duration = 30,
			requiredVotes = 0.6,
		}, self.options.voteKick)
		if not finite(self.voteKick.duration) or self.voteKick.duration <= 0 or
			not finite(self.voteKick.requiredVotes) or self.voteKick.requiredVotes <= 0 or self.voteKick.requiredVotes > 1 then
			error("voteKick duration and requiredVotes are invalid", 2)
		end
		self.activeVote = nil
		self.state = "lobby"
		if not finite(self.playerValidationInterval) or self.playerValidationInterval <= 0 then
			error("playerValidationInterval must be positive", 2)
		end
		if not finite(self.maxPlayerSpeed) or self.maxPlayerSpeed <= 0 or
			not finite(self.positionTolerance) or self.positionTolerance < 0 then
			error("player movement limits are invalid", 2)
		end
		if not finite(self.maxCommandLength) or self.maxCommandLength < 1 then
			error("maxCommandLength must be positive", 2)
		end
		if self.options.validatePlayerMotion ~= nil and type(self.options.validatePlayerMotion) ~= "function" then
			error("validatePlayerMotion must be a function", 2)
		end
		for _, callbackName in ipairs({ "createPlayerData", "getLocalPlayerState", "onPositionCorrection" }) do
			if self.options[callbackName] ~= nil and type(self.options[callbackName]) ~= "function" then
				error(callbackName .. " must be a function", 2)
			end
		end
		self.maxCommandLength = math.floor(self.maxCommandLength)
		if self.role == "server" then
			self:_installDefaultCommands()
			if self.options.gameMode then self:setGameMode(self.options.gameMode) end
		end
	end

	function sessionMT:_allowRate(peer, category, limit, window)
		if not peer then return false end
		local peerBuckets = self.rateLimits[peer]
		if not peerBuckets then peerBuckets = {}; self.rateLimits[peer] = peerBuckets end
		local bucket = peerBuckets[category]
		local time = self.clock
		if not bucket or time - bucket.started >= window then
			bucket = { started = time, count = 0 }
			peerBuckets[category] = bucket
		end
		bucket.count = bucket.count + 1
		if bucket.count <= limit then return true end
		local player = self.players[peer]
		self:_emit("RateLimitExceeded", player, category)
		return false
	end

	function sessionMT:_send(peer, message, reliable)
		if not peer then return false end
		local bytes = pack(message)
		if #bytes > self.maxPacketBytes then error("multiplayer packet exceeds configured size limit", 2) end
		local result = peer:send(bytes, 0, reliable and "reliable" or "unreliable")
		if result < 0 then error("ENet failed to queue a multiplayer packet", 2) end
		return true
	end

	function sessionMT:_service()
		for _ = 1, 256 do
			local event = self.host:service(0)
			if not event then break end
			if event.type == "receive" and type(event.data) ~= "string" then
				self:_emit("ProtocolViolation", event.peer, "invalid packet type")
			elseif event.type == "receive" and #event.data > self.maxPacketBytes then
				self:_emit("ProtocolViolation", event.peer, "packet too large")
			else
				if event.type == "connect" then
					if self.role == "client" then
						self.peer = event.peer
						self.helloSent = true
						self:_send(self.peer, {
							type = "hello",
							mode = self.mode,
							profile = clientProfilePayload(self.options),
						}, true)
					else
							self:_emit("PeerConnected", event.peer)
					end
				elseif event.type == "disconnect" then
					if self.role == "server" then
						self.peers[event.peer] = nil
						self:_removePlayer(event.peer, "disconnect")
					elseif self.peer == event.peer then
						self.peer, self.ready = nil, false
						self:_emit("Disconnected")
					end
				elseif event.type == "receive" and event.data then
					self:_receive(event.peer, unpack(event.data))
				end
			end
		end
	end

	function sessionMT:removePlayer(playerId, reason)
		if self.role ~= "server" then error("only the server can remove players", 2) end
		local player = self:getPlayer(playerId)
		if not player then return false end
		if player.peer.disconnect then player.peer:disconnect(1) end
		return self:_removePlayer(player.peer, reason or "kicked")
	end

	local function voteEligibleCount(session, targetId)
		local count = 0
		for _, player in ipairs(session:getPlayers()) do
			if player.id ~= targetId then count = count + 1 end
		end
		return count
	end

	function sessionMT:_broadcastVote()
		local vote = self.activeVote
		if not vote then return end
		local total, yes, no = 0, 0, 0
		for _ in pairs(vote.eligible) do total = total + 1 end
		for _, value in pairs(vote.votes) do
			if value then yes = yes + 1 else no = no + 1 end
		end
		self:_broadcast({
			type = "vote_state",
			targetId = vote.targetId,
			yes = yes,
			no = no,
			total = total,
			endsAt = vote.endsAt,
		}, true)
		self:_emit("VoteKickUpdated", vote.targetId, yes, no, total)
	end

	function sessionMT:_finishVote(success, reason)
		local vote = self.activeVote
		if not vote then return false end
		self.activeVote = nil
		self:_broadcast({
			type = "vote_result",
			targetId = vote.targetId,
			success = success == true,
			reason = reason or (success and "passed" or "failed"),
		}, true)
		self:_emit("VoteKickFinished", vote.targetId, success == true, reason)
		if success then self:removePlayer(vote.targetId, "vote-kicked") end
		return true
	end

	function sessionMT:startVoteKick(targetId, callerId)
		if self.role ~= "server" then error("only the server can start a vote kick", 2) end
		if not self.voteKick.enabled then return false, "vote kick is disabled" end
		if self.activeVote then return false, "a vote is already in progress" end
		local target = self:getPlayer(targetId)
		if not target then return false, "player not found" end
		local eligibleCount = voteEligibleCount(self, targetId)
		if eligibleCount == 0 then return false, "no eligible voters" end
		local threshold = self.voteKick.requiredVotes
		if not finite(threshold) or threshold <= 0 or threshold > 1 then
			error("voteKick.requiredVotes must be between 0 and 1", 2)
		end
		local eligible = {}
		for _, player in ipairs(self:getPlayers()) do
			if player.id ~= targetId then eligible[player.id] = true end
		end
		local vote = {
			targetId = targetId,
			eligible = eligible,
			votes = {},
			required = math.max(1, math.ceil(eligibleCount * threshold)),
			endsAt = self.clock + self.voteKick.duration,
		}
		self.activeVote = vote
		if callerId and eligible[callerId] then vote.votes[callerId] = true end
		self:_broadcastVote()
		self:_emit("VoteKickStarted", targetId, callerId)
		if (function() local n = 0; for _, yes in pairs(vote.votes) do if yes then n = n + 1 end end; return n end)() >= vote.required then
			self:_finishVote(true, "passed")
		end
		return true
	end

	function sessionMT:_startVoteFromPeer(peer, message)
		local player = self.players[peer]
		if not player or not self:_allowRate(peer, "vote_start", 1, 10) then return true end
		local ok, reason = self:startVoteKick(message.targetId, player.id)
		if not ok then sendCommandResult(self, peer, false, reason) end
		return true
	end

	function sessionMT:_castVoteFromPeer(peer, message)
		local player = self.players[peer]
		local vote = self.activeVote
		if not player or not vote or not vote.eligible[player.id] or
			type(message.yes) ~= "boolean" or vote.votes[player.id] ~= nil or
			not self:_allowRate(peer, "vote_cast", 2, 5) then return true end
		vote.votes[player.id] = message.yes
		self:_broadcastVote()
		local yes, cast = 0, 0
		for _, value in pairs(vote.votes) do cast = cast + 1; if value then yes = yes + 1 end end
		if yes >= vote.required then
			self:_finishVote(true, "passed")
		elseif yes + (voteEligibleCount(self, vote.targetId) - cast) < vote.required then
			self:_finishVote(false, "not enough votes")
		end
		return true
	end

	function sessionMT:requestVoteKick(targetId)
		if self.role ~= "client" then error("requestVoteKick is a client operation", 2) end
		if not self.peer or not self.ready or not validId(targetId) then return false end
		self:_send(self.peer, { type = "vote_request", targetId = targetId }, true)
		return true
	end

	function sessionMT:castVote(yes)
		if self.role ~= "client" then error("castVote is a client operation", 2) end
		if not self.peer or not self.ready or type(yes) ~= "boolean" then return false end
		self:_send(self.peer, { type = "vote_cast", yes = yes }, true)
		return true
	end

	function sessionMT:_receiveVoteMessage(message)
		if message.type == "vote_state" then
			self.voteState = clone(message)
		else
			self.voteState = nil
		end
		self:_emit(message.type == "vote_state" and "VoteKickUpdated" or "VoteKickFinished", clone(message))
	end

	function sessionMT:_updateFramework(dt)
		if self.role == "server" then
			if self.activeVote and self.clock >= self.activeVote.endsAt then
				local yes = 0
				for _, value in pairs(self.activeVote.votes) do if value then yes = yes + 1 end end
				self:_finishVote(yes >= self.activeVote.required, yes >= self.activeVote.required and "passed" or "expired")
			end
			if self.gameMode and self.gameMode.update then self.gameMode:update(self, dt) end
			self:_emit("Update", dt)
		end
	end

)netmoderation"
