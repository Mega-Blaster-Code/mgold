R"netplayersync(
	function sessionMT:_receiveServerFramework(peer, message)
		if message.type == "player_state_response" then
			local player = self.players[peer]
			local proposed = stateCopy(message.state)
			if not player or not proposed or not self:_allowRate(peer, "player_state", 8, 1) then return end
			if self.clock < player.syncUntil then
				self:_send(peer, { type = "player_state", id = player.id, state = player.state, correction = true }, true)
				return true
			end
			local validate = self.options.validatePlayerMotion
			local accepted, adjusted = false, nil
			if validate then
				local result = validate(player, clone(proposed), self.mode)
				if type(result) == "table" then adjusted = stateCopy(result); accepted = adjusted ~= nil
				else accepted = result == true end
			else
				local old = player.state.position
				local nextPosition = proposed.position
				local dx, dy, dz = nextPosition.x - old.x, nextPosition.y - old.y, nextPosition.z - old.z
				local distance = math.sqrt(dx * dx + dy * dy + dz * dz)
				local velocity = proposed.velocity
				local speed = math.sqrt(velocity.x * velocity.x + velocity.y * velocity.y + velocity.z * velocity.z)
				local elapsed = math.max(self.clock - player.lastAcceptedAt, self.playerValidationInterval)
				accepted = speed <= self.maxPlayerSpeed and
					distance <= self.maxPlayerSpeed * elapsed + self.positionTolerance
				if accepted and self.mode == "3d" and player.body then
					local hit = player.body:collisionTest({ x = dx, y = dy, z = dz })
					accepted = hit == nil
				end
			end
			if not accepted then
				player.syncUntil = self.clock + self.syncGrace
				self:_send(peer, { type = "player_state", id = player.id, state = player.state, correction = true }, true)
				self:_emit("InvalidPlayerMovement", player, clone(proposed))
				return true
			end
			player.state = adjusted or proposed
			setPlayerBodyState(player, player.state)
			player.lastAcceptedAt = self.clock
			if adjusted then
				player.syncUntil = self.clock + self.syncGrace
				self:_send(peer, {
					type = "player_state",
					id = player.id,
					state = clone(player.state),
					correction = true,
				}, true)
			end
			self:_broadcast({ type = "player_state", id = player.id, state = clone(player.state) }, true, peer)
			self:_emit(adjusted and "PlayerPositionCorrected" or "PlayerState", player, clone(player.state))
			return true
		elseif message.type == "player_data_request" then
			local player = self.players[peer]
			if player then self:_sendPlayerData(player, peer) end
			return true
		end
		if self:_receiveServerFeatures(peer, message) then return true end
		self:_emit("ProtocolViolation", peer, "unknown message type")
		return true
	end

	function sessionMT:_receiveClientFramework(message)
		if self:_receivePlayerFramework(message) then return end
		if message.type == "welcome" then
			self.playerId = message.playerId
			self.player = self.playersById[self.playerId]
		elseif message.type == "player_state_request" then
			local state = self.localPlayerState
			if self.options.getLocalPlayerState then state = self.options.getLocalPlayerState() end
			state = stateCopy(state)
			if state and self.peer then
				self:_send(self.peer, { type = "player_state_response", state = state }, true)
			end
		end
		self:_receiveClientFeatures(message)
	end
)netplayersync"
