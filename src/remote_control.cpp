#include "remote_control.h"
#include "HardwareSerial.h"
#include "water_sensors.h"
#include "device_config.h"
#include "gsm.h"
#include "sleep.h"
#include "http_request.h"
#include "globals.h"
#include <TinyGsmClient.h>
#include <Update.h>
#include "utils.h"
#include "ota.h"
#include "call_home.h"
#include "test_utils.h"

/******************************************************************************
 * Routines for controlling the device remotely through thingsboard.
 * Responsible for requesting remote control data from TB and then executing
 * the requested configs/actions.
 *****************************************************************************/
namespace RemoteControl
{
	//
	// Private funcs
	//
	RetResult json_to_data_struct(const JsonObject &json, RemoteControl::Data *data);

	RetResult handle_user_config(JsonObject json);
	RetResult handle_reboot(JsonObject json);
	void set_reboot_pending(bool val);


	/** 
	 * Set when reboot command is received in remote control data so that device can be 
	 * rebooted when calling home handling ends.
	 */
	bool _reboot_pending = false;

	/******************************************************************************
	 * Handle remote control
	 * First some of the current settings are published as client attributes to the
	 * device in TB.
	 * Then remote control data is requested (which are shared attributes) and applied
	 *****************************************************************************/
	RetResult start()
	{
		Serial.println(F("Remote control handling."));

		//
		// Send request
		//
		char url[URL_BUFFER_SIZE_LARGE] = "";

		Utils::tb_build_attributes_url_path(url, sizeof(url));

		HttpRequest http_req(GSM::get_modem(), TB_SERVER);
		http_req.set_port(TB_PORT);
				
		RetResult ret = RET_ERROR;

		Serial.println(F("Getting TB shared attributes."));
		ret = http_req.get(url, g_resp_buffer, sizeof(g_resp_buffer));

		if(ret != RET_OK)
		{
			Serial.println(F("Could not send request for remote control data."));
			
			Log::log(Log::RC_REQUEST_FAILED, http_req.get_response_code());

			return RET_ERROR;
		}

		Serial.println(F("Remote control raw data:"));
		Serial.println(F(g_resp_buffer));

		//
		// Deserialize received data
		//
		StaticJsonDocument<REMOTE_CONTROL_JSON_DOC_SIZE> json_remote;
		DeserializationError error = deserializeJson(json_remote, g_resp_buffer, strlen(g_resp_buffer));

		// Could not deserialize
		if(error)
		{
			Utils::serial_style(STYLE_RED);
			Serial.println(F("Could not deserialize received JSON, aborting."));
			Utils::serial_style(STYLE_RESET);
			
			Log::log(Log::RC_PARSE_FAILED);

			return RET_ERROR;
		}

		Serial.println(F("Remote control JSON: "));
		serializeJsonPretty(json_remote, Serial);
		Serial.println();

		// Get shared attributes key
		if(!json_remote.containsKey("shared"))
		{
			Serial.println(F("Returned JSON has no remote attributes."));
			Log::log(Log::RC_INVALID_FORMAT);
			
			return RET_ERROR;
		}
		JsonObject json_shared = json_remote["shared"];

		// At least id key must be present
		if(!json_shared.containsKey(TB_KEY_REMOTE_CONTROL_DATA_ID))
		{
			Serial.println(F("Returned JSON has no data id."));
			Log::log(Log::RC_INVALID_FORMAT);

			return RET_ERROR;
		}

		//
		// Check if ID is new
		// If remote control ID is old, remote control data is ignored
		// ID is new when it is different than the one stored from the previous remote control
		long long new_data_id = (int)json_shared[TB_KEY_REMOTE_CONTROL_DATA_ID];

		Serial.print(F("Remote control data id: Current "));
		Serial.print(DeviceConfig::get_last_rc_data_id());
		Serial.print(F(" - Received "));
		Serial.println((int)new_data_id);

		// Is it new?
		if(new_data_id == DeviceConfig::get_last_rc_data_id())
		{
			Utils::serial_style(STYLE_RED);
			Serial.println(F("Received remote control data is old, ignoring."));
			Utils::serial_style(STYLE_RESET);	
			return RET_OK;
		}
		else
		{
			Log::log(Log::RC_APPLYING_NEW_DATA, new_data_id, DeviceConfig::get_last_rc_data_id());

			// Data id is new, update in config to avoid rc data from being applied every time
			DeviceConfig::set_last_rc_data_id(new_data_id);
			DeviceConfig::commit();
		}

		Utils::serial_style(STYLE_BLUE);
		Serial.println(F("Received new remote control data. Applying..."));
		Utils::serial_style(STYLE_RESET);

		//
		// Handle/apply
		//

		// Handle user config
		RemoteControl::handle_user_config(json_shared);

		// Handle OTA if OTA requested
		if(json_shared.containsKey(TB_KEY_DO_OTA) && ((bool)json_shared[TB_KEY_DO_OTA]) == true)
		{
			OTA::handle_rc_data(json_shared);

			TestUtils::print_stack_size();
			
			// Send logs to report OTA events
			CallHome::handle_logs();
		}

		// // Handle reboot
		RemoteControl::handle_reboot(json_shared);

		return RET_OK;
	}

	/******************************************************************************
	 * User config
	 *****************************************************************************/
	RetResult handle_user_config(JsonObject json)
	{
		Serial.println(F("Applying new user config."));

		//
		// Water sensors measure interval
		//
		if(json.containsKey(TB_KEY_MEASURE_WATER_SENSORS_INT))
		{
			int ws_int = (int)json[TB_KEY_MEASURE_WATER_SENSORS_INT];

			Serial.print(F("Water sensors read interval: "));
			Serial.print(F("Current "));
			Serial.print(DeviceConfig::get_wakeup_schedule_reason_int(Sleep::WakeupReason::REASON_READ_WATER_SENSORS));
			Serial.print(F(" - New "));
			Serial.print(ws_int);
			Serial.println(F(". "));

			if(ws_int < MEASURE_WATER_SENSORS_INT_MINS_MIN || ws_int > MEASURE_WATER_SENSORS_INT_MINS_MAX)
			{
				Serial.println(F("Invalid value, ignoring."));
				Log::log(Log::RC_WATER_SENSORS_READ_INT_SET_FAILED, ws_int);
			}
			else
			{
				DeviceConfig::set_wakeup_schedule_reason_int(Sleep::WakeupReason::REASON_READ_WATER_SENSORS, ws_int);
				Serial.println(F("Applied."));

				Log::log(Log::RC_WATER_SENSORS_READ_INT_SET_SUCCESS, ws_int);
			}			
		}

		//
		// Call home interval
		//
		if(json.containsKey(TB_KEY_CALL_HOME_INT))
		{
			int ch_int = (int)json[TB_KEY_CALL_HOME_INT];

			Serial.print(F("Call home interval: "));
			Serial.print(F("Current "));
			Serial.print(DeviceConfig::get_wakeup_schedule_reason_int(Sleep::WakeupReason::REASON_CALL_HOME));
			Serial.print(F(" - New "));
			Serial.println(ch_int);

			if(ch_int < CALL_HOME_INT_MINS_MIN || ch_int > CALL_HOME_INT_MINS_MAX)
			{
				Serial.println(F("Invalid value, ignoring."));
				Log::log(Log::RC_CALL_HOME_INT_SET_FAILED, ch_int);
			}
			else
			{
				DeviceConfig::set_wakeup_schedule_reason_int(Sleep::WakeupReason::REASON_CALL_HOME, ch_int);
				Serial.println(F("Applied."));

				Log::log(Log::RC_CALL_HOME_INT_SET_SUCCESS, ch_int);
			}			
		}

		DeviceConfig::print_current();

		// Commit all changes
		DeviceConfig::commit();

		// Handle SPIFFS format
		if(json.containsKey(TB_KEY_FORMAT_SPIFFS) && ((bool)json[TB_KEY_FORMAT_SPIFFS]) == true)
		{
			Serial.println(F("Formatting SPIFFS"));

			// TODO: Submit logs before formatting SPIFFS?

			int bytes_before_format = SPIFFS.usedBytes();

			if(SPIFFS.format())
			{
				Serial.println(F("Format complete"));

				Log::log(Log::SPIFFS_FORMATTED, bytes_before_format);
			}
			else
			{
				Serial.println(F("Format failed!"));

				// Try to log in case format failed but FS still accessible
				Log::log(Log::SPIFFS_FORMAT_FAILED);
			}
		}

		return RET_OK;
	}

	/******************************************************************************
	 * Mark device pending flag so device reboots when calling home handling ends
	 *****************************************************************************/
	RetResult handle_reboot(JsonObject json)	
	{	
		if(json.containsKey(TB_KEY_DO_REBOOT))
		{
			bool do_reboot = (bool)json[TB_KEY_DO_REBOOT];
			if(do_reboot)
			{
				Utils::serial_style(STYLE_BLUE);
				Serial.println(F("Reboot requested. Device will be rebooted when calling home handling ends."));
				Utils::serial_style(STYLE_RESET);

				// Mark as reboot pending and the device will be rebooted after all processes finish
				set_reboot_pending(true);
			}
		}

		return RET_OK;
	}

	/******************************************************************************
	* Get/set reboot pending flag
	* When a reboot is required after calling home handling finishes (eg. to apply
	* some of the required settings), it must set this flag to make CallingHome
	* reboot the device when its done.
	******************************************************************************/
	bool get_reboot_pending()
	{
		return _reboot_pending;
	}
	void set_reboot_pending(bool val)
	{
		_reboot_pending = val;
	}

	/******************************************************************************
	* Print data
	******************************************************************************/
	void print(const Data *data)
	{
		Serial.print(F("ID: "));
		Serial.println(data->id, DEC);

		Serial.print(F("Water sensors measure int (mins): "));
		Serial.println(data->water_sensors_measure_int_mins, DEC);

		Serial.print(F("Call home int(mins): "));
		Serial.println(data->call_home_int_mins, DEC);

		Serial.print(F("Reboot: "));
		Serial.println(data->reboot, DEC);

		Serial.print(F("OTA: "));
		Serial.println(data->ota, DEC);
	}
}