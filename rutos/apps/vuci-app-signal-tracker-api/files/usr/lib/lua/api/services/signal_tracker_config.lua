-- UCI configuration endpoint for /etc/config/signal_tracker (single section 'main').
-- Cross-field rules (min < max, fine <= coarse, ...) are checked by the daemon, which
-- reports them in the status 'config' block and refuses to move until they are fixed.
local ConfigService = require("api/ConfigService")

local SignalTrackerConfig = ConfigService:new({
	delete = false,
	create = false,
})

local Main = SignalTrackerConfig:section(
	"signal_tracker", -- UCI config name
	"signal_tracker"  -- UCI section type
)
Main:make_primary()

local function integer_option(name, min, max)
	local option = Main:option(name)
	function option:validate(value)
		local n = tonumber(value)
		if not n or n ~= math.floor(n) or n < min or n > max then
			return false, string.format("Must be an integer between %d and %d", min, max)
		end
		return true
	end
	return option
end

local function number_option(name, min, max)
	local option = Main:option(name)
	function option:validate(value)
		local n = tonumber(value)
		if not n or n < min or n > max then
			return false, string.format("Must be a number between %s and %s", min, max)
		end
		return true
	end
	return option
end

local function bool_option(name)
	local option = Main:option(name)
	function option:validate(value)
		return self.dt:is_bool(value)
	end
	return option
end

bool_option("enabled")
integer_option("min_angle", 0, 360)
integer_option("max_angle", 0, 360)
integer_option("coarse_step", 1, 360)
integer_option("fine_step", 1, 360)
integer_option("settle_ms", 0, 120000)
integer_option("samples", 1, 20)
integer_option("sample_interval_ms", 0, 60000)
integer_option("min_valid_samples", 1, 20)
integer_option("move_timeout_ms", 500, 60000)
integer_option("request_timeout_ms", 200, 10000)
number_option("min_improvement", 0, 100)
number_option("weight_rsrp", 0, 10)
number_option("weight_rsrq", 0, 10)
number_option("weight_sinr", 0, 10)
bool_option("simulate")
integer_option("sim_best_angle", 0, 360)

local device = Main:option("device")
	device.maxlength = 63
	function device:validate(value)
		if value == "auto" or value:match("^/dev/[%w%._%-/]+$") then
			return true
		end
		return false, "Must be 'auto' or a /dev path"
	end

local modem = Main:option("modem")
	modem.maxlength = 31
	function modem:validate(value)
		if value == "" or value:match("^[%d%.%-]+$") then
			return true
		end
		return false, "Must be a modem USB id such as 1-1"
	end

local usb_id = Main:option("usb_id", { list = true })
	usb_id.maxlength = 9

return SignalTrackerConfig
