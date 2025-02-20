#include <Arduino.h>
#include <stdlib.h>
#include <ctime>
#include <sys/time.h>
#include "common.h"
#include "call_home.h"
#include "const.h"
#include "flash.h"
#include "globals.h"
#include "water_sensor_data.h"
#include "utils.h"
#include "gsm.h"
#include "rtc.h"
#include "log.h"
#include "tests.h"
#include "test_utils.h"
#include "sleep_scheduler.h"
#include "water_sensors.h"
#include <Wire.h>
#include <RtcDS3231.h>
#include "device_config.h"
#include "battery.h"
#include "int_env_sensor.h"
#include <SPIFFS.h>
#include "ota.h"
#include "atmos41.h"
#include "rom/rtc.h"
#include "fo_sniffer.h"
#include "fo_uart.h"
#include "fo_data.h"
#include "config_mode.h"
#include "driver/rtc_io.h"
#include "esp_sleep.h"
#include "lightning.h"
#include "battery_gauge.h"
#include "solar_monitor.h"
#include "ipfs_client.h"

// For testing
#include "http_request.h"
#include "sleep_scheduler.h"
#include "data_store_reader.h"
#include "sdi12_log.h"
#include "tb_sdi12_log_json_builder.h"
#include "wifi_modem.h"
#include "sdi12.h"
#include "sdi12_sensor.h"
#include "dfrobot_liquid.h"
#include "teros12.h"
#include "soil_moisture_data.h"
#include "water_level.h"
#include "water_presence.h"
#include "aquatroll.h"

/******************************************************************************
 * Setup
 *****************************************************************************/
void setup() 
{
	Serial.begin(115200);
		
	Utils::serial_style(STYLE_BLUE);
	Utils::print_separator(F("BOOTING"));
	Utils::serial_style(STYLE_RESET);
	debug_print(F("\n\n"));

	//
	// Print on-boot info
	// 
	DeviceConfig::init();

	Utils::serial_style(STYLE_GREEN);
	debug_print(F("Reset reason: "));
	Utils::print_reset_reason();
	debug_println();
	Utils::serial_style(STYLE_RESET);


	#if DEBUG
		Utils::serial_style(STYLE_RED);
		Utils::print_block(F("Debug build"));
		Utils::serial_style(STYLE_RESET);
		debug_println();
	#else
		Utils::serial_style(STYLE_BLUE);	
		Utils::print_block(F("Release build"));
		Utils::serial_style(STYLE_RESET);
		debug_println();
	#endif

	Utils::print_flags();
	debug_println();

	// Device info
	Utils::print_separator(F("DEVICE INFO"));

	// Board name
	debug_print(F("Board: "));
	debug_println(BOARD_NAME);

	debug_print(F("Version: "));
	debug_println(FW_VERSION, DEC);

	char mac[20] = "";
	Utils::get_mac(mac, sizeof(mac));
	debug_print(F("MAC: "));
	debug_println(mac);
	debug_print(F("APN: "));
	debug_println(DeviceConfig::get_cellular_apn());
	Serial.print(F("TB Server: "));
	Serial.println(TB_SERVER);
	debug_print(F("TB Access Token: "));
	debug_println(DeviceConfig::get_tb_device_token());

	if(FLAGS.WATER_QUALITY_SENSOR_ENABLED)
	{
		debug_print(F("AquaTROLL Model: "));

		if(AQUATROLL_MODEL == AQUATROLL_MODEL_400)
		{
			debug_println(F("400"));
		}
		else if(AQUATROLL_MODEL == AQUATROLL_MODEL_500)
		{
			debug_println(F("500"));
		}
		else if(AQUATROLL_MODEL == AQUATROLL_MODEL_600)
		{
			debug_println(F("600"));
		}
		else
		{
			debug_println_e(F("Invalid AquaTROLL model selected"));
		}
	}

	debug_println();

	// Print program info
	debug_print(F("Program size: "));
	debug_print(ESP.getSketchSize() / 1024, DEC);

	debug_println(F("KB"));
	debug_print(F("Free program space: "));
	debug_print(ESP.getFreeSketchSpace() / 1024, DEC);
	debug_println(F("KB"));

	debug_print(F("Free heap: "));
	debug_print(ESP.getFreeSketchSpace(), DEC);
	debug_println(F("B"));

	debug_print(F("CPU freq: "));
	debug_print(ESP.getCpuFreqMHz());
	debug_println("MHz");

	Utils::print_separator(NULL);
	debug_println();

	// Check if credentials are configured and if not use fallback
	Utils::check_credentials();

	// Turn lightning sensor off before config mode.
	// If IRQ was not cleared before device restart
	// Lightning::off();

	// Handle config mode if needed
	ConfigMode::handle();

	//
	// Init 
	// Order important

	// Init main I2C1 bus
	Wire.begin(PIN_I2C1_SDA, PIN_I2C1_SCL, 100000);

	// Init ext RTC first and sync system time
	RTC::init();
	RTC::enable_timechange_safety(false);
	RTC::sync_time_from_ext_rtc();
	RTC::enable_timechange_safety(true);

	#ifdef TCALL_H
		// Turn IP5306 power boost OFF to reduce idle current
		Utils::ip5306_set_power_boost_state(false);
	#endif

	delay(100);
	IntEnvSensor::init();
	Battery::init();
	BatteryGauge::init();
	SolarMonitor::init();
	delay(100);
	Flash::mount();
	Flash::ls();
	GSM::init();
	WaterSensors::init();
	WaterLevel::init();
	WaterPresence::init();
	Atmos41::init();

	if(FO_SOURCE == FO_SOURCE_SNIFFER)
	{
		Serial.println(F("FO source sniffer"));
		FoSniffer::init();

		// If no FO node id is set, scan
		if(DeviceConfig::get_fo_sniffer_id() == 0 && DeviceConfig::get_fo_enabled())
		{
			debug_println_e(F("No FO weather station id is set, scanning."));
			FoSniffer::scan_fo_id(true);
		}
	}
	else if(FO_SOURCE == FO_SOURCE_UART)
	{
		Serial.println(F("FO source UART"));
		FoUart::init();
	}
	
	// Log boot now that memory has been inited
	Log::log(Log::Code::BOOT, FW_VERSION, (int)rtc_get_reset_reason(0));

	// Log mac address
	Utils::log_mac();

	// Log and print battery
	Battery::log_adc();
	Battery::log_solar_adc();
	IntEnvSensor::log();

	// Log and print Battery gauge
	BatteryGauge::print();
	BatteryGauge::log();

	// Log and print Solar Monitor
	SolarMonitor::print();
	SolarMonitor::log();

	// Device info
	DeviceConfig::print_current();

	//
	// Boot tests
	//
	Utils::boot_self_test();

	// Check if boot is after OTA
	if(DeviceConfig::get_ota_flashed())
	{
		OTA::handle_first_boot();
	}

	//
	// Check if device needs to be in sleep mode in case this is after a brown out
	//
	Serial.println(F("Checking sleep charge"));
	Battery::sleep_charge();

	if(FLAGS.LIGHTNING_SENSOR_ENABLED)
	{
		if(Lightning::on() != RET_OK)
		{
			Log::log(Log::LIGHTNING_FAILED_TO_START, LIGHTNING_SENSOR_MODULE, LIGHTNING_I2C_ADDR);
		}
	}

	//
	// Check if reboot clean
	// Reboot is considered clean when the clean_boot flag is set
	// Otherwise the device was hard-reset or reboot was unexpected (exception)
	//
	if(DeviceConfig::get_clean_reboot())
	{
		Utils::serial_style(STYLE_GREEN);
		debug_println(F("Clean boot"));
		Utils::serial_style(STYLE_RESET);

		// Reset flag
		DeviceConfig::set_clean_reboot(false);
		DeviceConfig::commit();
	}
	else
	{
		Utils::serial_style(STYLE_RED);
		debug_println(F("Boot is not clean (not intentional)"));
		Utils::serial_style(STYLE_RESET);
	}

	//
	// Turn on GSM to check if SIM card present and sync time
	// In debug mode sync time from external RTC
	if(FLAGS.DEBUG_MODE && RTC::tstamp_valid(RTC::get_timestamp()))
	{
		debug_println(F("Debug mode, using ext RTC time."));
		RTC::sync_time_from_ext_rtc();
	}
	else
	{
		if(GSM::on() != RET_ERROR)
		{ 
			// 
				// Get SIM CCID to check for its presence
			// 
			if(!GSM::is_sim_card_present())
			{
				Utils::serial_style(STYLE_RED);
				debug_println(F("No SIM card detected!"));
				Utils::serial_style(STYLE_RESET);

				Log::log(Log::GSM_NO_SIM_CARD);
			}

			//
			// Sync RTC
			// On boot sync, disable timechange safety
			if(RTC::sync(false) != RET_OK) // RTC turns GSM ON
			{
				Utils::serial_style(STYLE_RED);
				debug_println(F("Failed to sync time, system has no source of time."));
				Utils::serial_style(STYLE_RESET);
			}
			else
			{
				debug_println(F("Time sync successful."));
				RTC::print_time();
			}
		}
	}

	//////////////////////////////////////////////////////////////////////////////////////////

	// TODO: Make all tasks run on boot and remove this
	if(FLAGS.WATER_QUALITY_SENSOR_ENABLED || FLAGS.WATER_LEVEL_SENSOR_ENABLED)
	{
		Utils::serial_style(STYLE_MAGENTA);
		debug_println(F("Reading: Read water sensors"));
		Utils::serial_style(STYLE_RESET);
		WaterSensors::log();
	}
	if(FLAGS.ATMOS41_ENABLED)
	{
		Utils::serial_style(STYLE_CYAN);
		debug_println(F("Reading: Read weather station"));
		Utils::serial_style(STYLE_RESET);
		Atmos41::measure_log();
	}
	if(FLAGS.SOIL_MOISTURE_SENSOR_ENABLED)
	{
		Utils::serial_style(STYLE_CYAN);
		debug_println(F("Reading: Read soil moisture sensor"));
		Utils::serial_style(STYLE_RESET);
		Teros12::log();
	}

	Utils::serial_style(STYLE_BLUE);
	debug_println(F("Reason: Call home"));
	Utils::serial_style(STYLE_RESET);
	CallHome::start();

	Utils::print_separator(F("SETUP COMPLETE"));
}

/******************************************************************************
 * Wake up self test
 * On each wake up, a self test checks if main parameters to see if it is OK
 * to proceed with normal operation (measure, send data etc.)
 * If not, regular wake up is not executed and the device goes back to sleep
 * until next wake up event.
 *****************************************************************************/
RetResult wakeup_self_test()
{
	RetResult ret = RET_OK;

	// Check if RTC returns invalid value
	int drift = RTC::detect_drift();
	if(!RTC::tstamp_valid(RTC::get_timestamp()) || drift > 0)
	{
		if(GSM::on() != RET_OK)
		{
			debug_println(F("Could not turn on GSM"));
			ret = RET_ERROR;
		}

		if(drift > 0)
		{
			debug_print(F("RTC drift detected: "));
			debug_println(drift, DEC);
			Log::log(Log::RTC_DRIFT_DETECTED, drift, RTC::get_external_rtc_timestamp());
		}
		else
		{
			debug_println_e(F("RTC returns invalid timestamp, syncing..."));
		}

		//
		// Sync RTC
		//	
		if(RTC::sync() != RET_OK)
		{
			debug_println(F("Failed to sync time."));
			ret = RET_ERROR;
		}
		else
		{
			debug_println(F("Time sync successful."));
			RTC::print_time();
		}
	}

	if(ret != RET_OK)
	{
		Log::log(Log::WAKEUP_SELF_TEST_FAILED);
	}

	return ret;
}

/******************************************************************************
* Main loop
******************************************************************************/
void loop()
{
	// Handle battery sleep charge if needed
	if(Battery::get_current_mode() == BATTERY_MODE::BATTERY_MODE_SLEEP_CHARGE)
	{
		Battery::sleep_charge();
	}

	//
	// Go to sleep
	//
	SleepScheduler::sleep_to_next();

	//
	// Wake up
	//

	// Woke up on IRQ from lightning sensor?
	// Handle otherwise go back to sleep
	if(FLAGS.LIGHTNING_SENSOR_ENABLED && esp_sleep_get_wakeup_cause() == ESP_SLEEP_WAKEUP_EXT0)
	{
		Lightning::handle_irq();
		return;
	}

	// Do not log when waking up for FO Sniff
	if(!SleepScheduler::wakeup_reason_is(SleepScheduler::REASON_FO))
	{
		IntEnvSensor::log();
	}

	// Do wake up self test
	if(wakeup_self_test() != RET_OK)
	{
		debug_println_e(F("Wake up self test failed, going back to sleep."));
	}
	else
	{
		//
		// Tasks executed only when wakeup self test passed
		//

		//
		// Sniff FO weather station
		//
		if(SleepScheduler::wakeup_reason_is(SleepScheduler::REASON_FO))
		{
			debug_println_i(F("Reason: Sniff FO weather station."));
			
			if(!DeviceConfig::get_fo_enabled())
			{
				debug_println_e(F("FO sniffer disabled, sniffing aborted."));
			}
			else
			{
				if(FO_SOURCE == FO_SOURCE_SNIFFER)
					FoSniffer::handle_sniff_event();
				else if(FO_SOURCE == FO_SOURCE_UART)
					FoUart::handle_scheduled_event();
			}
		}

		//
		// Measure water quality
		//
		if(SleepScheduler::wakeup_reason_is(SleepScheduler::REASON_READ_WATER_SENSORS))
		{
			debug_println_i(F("Reason: Read water sensors"));

			if(!FLAGS.WATER_QUALITY_SENSOR_ENABLED &&  !FLAGS.WATER_LEVEL_SENSOR_ENABLED)
			{
				debug_println_e(F("Water sensors disabled, measurement aborted."));
			}
			else
			{
				WaterSensors::log();
			}
		}

		//
		// Measure soil moisture
		//
		if(SleepScheduler::wakeup_reason_is(SleepScheduler::REASON_READ_SOIL_MOISTURE_SENSOR))
		{
			debug_println_i(F("Reason: Read soil moisture"));

			if(!FLAGS.SOIL_MOISTURE_SENSOR_ENABLED)
			{
				debug_println_e(F("Soil moisture sensor disabled, measurement aborted."));
			}
			else
			{
				Teros12::log();
			}
		}

		//
		// Measure weather data
		//
		if(SleepScheduler::wakeup_reason_is(SleepScheduler::REASON_READ_WEATHER_STATION))
		{
			debug_println_i(F("Reason: Read weather station"));

			if(!FLAGS.ATMOS41_ENABLED)
			{
				debug_println_e(F("Weather station disabled, measurement aborted."));
			}
			else
			{
				Atmos41::measure_log();
			}
		}
	}
	
	if(SleepScheduler::wakeup_reason_is(SleepScheduler::REASON_CALL_HOME))
	{
		debug_println_i(F("Reason: Call home"));
		CallHome::start();
	}

	debug_println(F("------------------------------------------------"));
}