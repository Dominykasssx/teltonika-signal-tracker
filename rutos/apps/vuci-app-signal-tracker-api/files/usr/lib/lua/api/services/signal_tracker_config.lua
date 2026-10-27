-- UCI configuration endpoint for /etc/config/signal_tracker: section 'general', which the
-- signal-tracker service writes with the default settings when it is missing.
local ConfigService = require("api/ConfigService")

local SignalTrackerConfig = ConfigService:new({
	delete = false,
	create = false,
})

local General = SignalTrackerConfig:section(
	"signal_tracker", -- UCI config name
	"signal_tracker"  -- UCI section type
)
General:make_primary()

local function integer_option(name, min, max)
	local option = General:option(name)
	function option:validate(value)
		local n = tonumber(value)
		if not n or n ~= math.floor(n) or n < min or n > max then
			return false, string.format("Must be a whole number between %d and %d", min, max)
		end
		return true
	end
end

integer_option("positions", 2, 26)
integer_option("settle", 0, 60)
integer_option("samples", 1, 10)

local metric = General:option("metric")
	function metric:validate(value)
		if value == "signal_quality" or value == "rsrp" then
			return true
		end
		return false, "Must be 'signal_quality' or 'rsrp'"
	end

-- gsmd's gsm.modem0 (primary) or gsm.modem1 (secondary)
local modem = General:option("modem")
	function modem:validate(value)
		if value == "0" or value == "1" then
			return true
		end
		return false, "Must be '0' (primary modem) or '1' (secondary modem)"
	end

local enabled = General:option("enabled")
	function enabled:validate(value)
		return self.dt:is_bool(value)
	end

local device = General:option("device")
	device.maxlength = 63
	function device:validate(value)
		if value == "auto" or value:match("^/dev/[%w%._%-/]+$") then
			return true
		end
		return false, "Must be 'auto' or a /dev path"
	end

return SignalTrackerConfig
