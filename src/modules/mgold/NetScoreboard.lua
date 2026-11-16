R"netscoreboard(
	local function copyScoreboardColumns(columns)
		if type(columns) ~= "table" then error("scoreboard columns must be an array", 3) end
		local result, seen = {}, {}
		for i, column in ipairs(columns) do
			if not validName(column, 64) then error("scoreboard column names must be non-empty strings", 3) end
			if not seen[column] then
				result[#result + 1] = column
				seen[column] = true
			end
		end
		return result
	end

	function sessionMT:setScoreboardColumns(columns)
		self.scoreboard.columns = copyScoreboardColumns(columns)
		return self
	end

	function sessionMT:getScoreboard()
		local rows = {}
		for _, player in ipairs(self:getPlayers()) do
			local keys, included = {}, {}
			for _, key in ipairs(self.scoreboard.columns) do
				local isPublic = self.role ~= "server" or player.visibility[key] == true
				if isPublic and player.data[key] ~= nil then
					keys[#keys + 1] = key
					included[key] = true
				end
			end
			local extraKeys = {}
			for key, value in pairs(player.data) do
				local isPublic = self.role ~= "server" or player.visibility[key] == true
				if isPublic and value ~= nil and not included[key] then
					extraKeys[#extraKeys + 1] = key
				end
			end
			table.sort(extraKeys)
			for _, key in ipairs(extraKeys) do keys[#keys + 1] = key end

			local row = {}
			for _, key in ipairs(keys) do row[#row + 1] = { key, clone(player.data[key]) } end
			rows[#rows + 1] = row
		end
		return rows
	end

	network.scoreboard = {
		create = function(columns)
			local board = { columns = copyScoreboardColumns(columns or { "name", "color" }) }
			function board:setColumns(nextColumns)
				self.columns = copyScoreboardColumns(nextColumns)
				return self
			end
			function board:getRows(session)
				if not session or not session.getScoreboard then error("scoreboard:getRows requires a multiplayer session", 2) end
				return session:getScoreboard()
			end
			function board:draw(session)
				local graphics, keyboard = love.graphics, love.keyboard
				if not graphics or not keyboard or type(keyboard.isDown) ~= "function" then
					error("scoreboard:draw requires love.graphics and love.keyboard", 2)
				end
				if not keyboard.isDown("tab") then return false end
				local rows = self:getRows(session)
				local available = { name = true }
				for _, row in ipairs(rows) do
					for _, pair in ipairs(row) do
						if pair[1] ~= "color" then available[pair[1]] = true end
					end
				end

				local columns, included = {}, {}
				for _, key in ipairs(self.columns) do
					if key ~= "color" and available[key] and not included[key] then
						columns[#columns + 1] = key
						included[key] = true
					end
				end
				local extraKeys = {}
				for key in pairs(available) do
					if not included[key] then extraKeys[#extraKeys + 1] = key end
				end
				table.sort(extraKeys)
				for _, key in ipairs(extraKeys) do columns[#columns + 1] = key end

				local font = graphics.getFont()
				local padding, spacing, rowHeight, headerHeight = 15, 25, 24, 38
				local widths = {}
				for _, key in ipairs(columns) do widths[key] = font:getWidth(tostring(key):upper()) end
				for _, row in ipairs(rows) do
					local values = {}
					for _, pair in ipairs(row) do values[pair[1]] = pair[2] end
					for _, key in ipairs(columns) do
						local value = values[key]
						if value ~= nil then
							widths[key] = math.max(widths[key], font:getWidth(tostring(value)))
						end
					end
				end

				local positions, panelWidth = {}, padding * 2
				for _, key in ipairs(columns) do
					positions[key] = panelWidth
					panelWidth = panelWidth + widths[key] + spacing
				end
				panelWidth = math.max(1, panelWidth - spacing) + padding
				local screenWidth, screenHeight = graphics.getDimensions()
				local panelHeight = headerHeight + math.max(#rows, 1) * rowHeight
				local x, y = (screenWidth - panelWidth) / 2, (screenHeight - panelHeight) / 2

				graphics.push("all")
				graphics.setColor(0, 0, 0, 0.88)
				graphics.rectangle("fill", x, y, panelWidth, panelHeight, 3, 3)
				graphics.setColor(0.12, 0.28, 0.55, 1)
				graphics.rectangle("fill", x, y, panelWidth, headerHeight, 3, 3)
				graphics.setColor(1, 1, 1, 1)
				for _, key in ipairs(columns) do
					graphics.print(tostring(key):upper(), x + positions[key], y + 12)
				end
				for index, row in ipairs(rows) do
					local rowY = y + headerHeight + (index - 1) * rowHeight
					if index % 2 == 0 then
						graphics.setColor(0.12, 0.12, 0.12, 0.9)
						graphics.rectangle("fill", x + 1, rowY, panelWidth - 2, rowHeight)
					end
					local values = {}
					for _, pair in ipairs(row) do values[pair[1]] = pair[2] end
					local color = values.color or values.roleColor
					local r, g, b, a = 1, 1, 1, 1
					if type(color) == "table" then
						r = tonumber(color[1] or color.r) or r
						g = tonumber(color[2] or color.g) or g
						b = tonumber(color[3] or color.b) or b
						a = tonumber(color[4] or color.a) or a
					end
					for _, key in ipairs(columns) do
						local value = values[key]
						if value ~= nil then
							if key == "name" then graphics.setColor(1, 1, 1, 1)
							else graphics.setColor(r, g, b, a) end
							graphics.print(tostring(value), x + positions[key], rowY + 5)
						end
					end
				end
				graphics.pop()
				return true
			end
			return board
		end,
	}

	local engineScoreboard = network.scoreboard.create()

	function sessionMT:setScoreboardEnabled(enabled)
		if type(enabled) ~= "boolean" then error("scoreboard enabled state must be a boolean", 2) end
		self.scoreboard.enabled = enabled
		return self
	end

	function sessionMT:isScoreboardEnabled()
		return self.scoreboard.enabled
	end

	function sessionMT:drawScoreboard()
		engineScoreboard:setColumns(self.scoreboard.columns)
		return engineScoreboard:draw(self)
	end

	local scoreboardDrawWrapper
	function sessionMT:_ensureScoreboardDrawHook()
		if type(love) ~= "table" then return end
		local currentDraw = love.draw
		if currentDraw == scoreboardDrawWrapper then return end
		if currentDraw ~= nil and type(currentDraw) ~= "function" then
			error("love.draw must be a function to enable the built-in scoreboard", 2)
		end

		local gameDraw = currentDraw
		scoreboardDrawWrapper = function(...)
			if gameDraw then gameDraw(...) end
			local session = activeSession
			if session and session.role == "client" and session:isScoreboardEnabled() then
				session:drawScoreboard()
			end
		end
		love.draw = scoreboardDrawWrapper
	end
)netscoreboard"
