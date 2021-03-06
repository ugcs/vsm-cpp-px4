// Copyright (c) 2018, Smart Projects Holdings Ltd
// All rights reserved.
// See LICENSE file for license details.

#include <px4_vehicle.h>

constexpr float Px4_vehicle::MAX_COPTER_SPEED;

using namespace ugcs::vsm;

constexpr std::chrono::milliseconds Px4_vehicle::MANUAL_CONTROL_PERIOD;
constexpr std::chrono::milliseconds Px4_vehicle::MANUAL_CONTROL_TIMEOUT;

// Constructor for command processor.
Px4_vehicle::Px4_vehicle(proto::Vehicle_type type):
        Mavlink_vehicle(Vendor::PX4, "px4", type),
        vehicle_command(*this),
        task_upload(*this),
        set_poi_supported(true)
{
    switch (type) {
    case proto::VEHICLE_TYPE_HELICOPTER:
    case proto::VEHICLE_TYPE_MULTICOPTER:
        Set_model_name("PX4Copter");
        break;
    default:
        VSM_EXCEPTION(Internal_error_exception, "unsupported type");
    }
}

void
Px4_vehicle::On_enable()
{
    Configure_common();

    if (device_type == proto::DEVICE_TYPE_VEHICLE_COMMAND_PROCESSOR) {
        // Do not need any other initialization for command_processor.
        // Just register it with UCS.
        Register();
        // Send command availability.
        Commit_to_ucs();
        return;
    }

    Configure_real_vehicle();
    // Get parameter values.
    Mavlink_vehicle::On_enable();

    c_mission_upload->Set_available();
    c_arm->Set_available();
    c_land_command->Set_available();
    c_emergency_land->Set_available();
    c_disarm->Set_available();
    c_waypoint->Set_available();
    c_auto->Set_available();
    c_manual->Set_available();
    c_guided->Set_available();
    c_pause->Set_available();
    c_resume->Set_available();
    c_rth->Set_available();
    c_takeoff_command->Set_available();
    c_direct_payload_control->Set_available();
    c_payload_control->Set_available();
    c_joystick->Set_available();
    c_direct_payload_control->Set_enabled(true);

    c_transition_fixed = flight_controller->Add_command("transition_fixed", true);
    c_transition_vtol = flight_controller->Add_command("transition_vtol", true);

    c_set_poi->Set_available();
    c_set_poi->Set_enabled();

    Commit_to_ucs();    // push state info.

    if (use_mavlink_2 && *use_mavlink_2) {
        mav_stream->Set_mavlink_v2(true);
    }

    read_waypoints.item_handler = Read_waypoints::Make_mission_item_handler(
        &Px4_vehicle::On_mission_item,
        Shared_from_this());

    read_waypoints.Set_next_action(
        Write_parameters::Make_next_action(
            &Px4_vehicle::On_mission_downloaded,
            Shared_from_this()));

    // Register handlers for messages which should be disabled.
    // Cannot do this in Default handler because mav_stream can be shared by multiple vehicles.
    #define REG_DISABLER(x) \
    common_handlers.Register_mavlink_handler<mavlink::x>(&Px4_vehicle::Disable_message_on_receive<mavlink::x>, this)

    REG_DISABLER(HIGHRES_IMU);
    REG_DISABLER(ATTITUDE_TARGET);
    REG_DISABLER(ATTITUDE_QUATERNION);
    REG_DISABLER(ACTUATOR_CONTROL_TARGET);
    REG_DISABLER(TIMESYNC);
    REG_DISABLER(POSITION_TARGET_LOCAL_NED);
    REG_DISABLER(SERVO_OUTPUT_RAW);
    REG_DISABLER(WIND_COV);
    REG_DISABLER(VIBRATION);

    // Home location handler.
    common_handlers.Register_mavlink_handler<mavlink::MESSAGE_ID::HOME_POSITION>(
        &Px4_vehicle::On_home_position,
        this);

    // To detect takeoff.
    common_handlers.Register_mavlink_handler<mavlink::MESSAGE_ID::EXTENDED_SYS_STATE>(
        &Px4_vehicle::On_extended_sys_state,
        this);

    common_handlers.Register_mavlink_handler<mavlink::MESSAGE_ID::AUTOPILOT_VERSION>(
        &Px4_vehicle::On_autopilot_version,
        this);

    common_handlers.Register_mavlink_handler<mavlink::MESSAGE_ID::CAMERA_INFORMATION>(
        &Px4_vehicle::On_camera_information,
        this);

    common_handlers.Register_mavlink_handler<mavlink::MESSAGE_ID::CAMERA_IMAGE_CAPTURED>(
        &Px4_vehicle::On_image_captured,
        this);

    common_handlers.Register_mavlink_handler<mavlink::MESSAGE_ID::PARAM_VALUE>(
        &Px4_vehicle::On_parameter,
        this);

    // Get autopilot version
    auto cmd_long = mavlink::Pld_command_long::Create();
    (*cmd_long)->target_component = real_component_id;
    (*cmd_long)->target_system = real_system_id;
    (*cmd_long)->command = mavlink::MAV_CMD::MAV_CMD_REQUEST_AUTOPILOT_CAPABILITIES;
    (*cmd_long)->param1 = 1;    // request version.
    (*cmd_long)->confirmation = 0;

    if (use_mavlink_2) {
        Send_message(*cmd_long);
    } else {
        // Send request in both formats. On response VSM will settle on mavlink version.
        Send_message_v1(*cmd_long);
        Send_message_v2(*cmd_long);
    }
}

void
Px4_vehicle::On_disable()
{
    if (device_type == proto::DEVICE_TYPE_VEHICLE_COMMAND_PROCESSOR) {
        return;
    }
    if (direct_vehicle_control_timer) {
        direct_vehicle_control_timer->Cancel();
    }
    read_waypoints.item_handler = Read_waypoints::Mission_item_handler();
    Mavlink_vehicle::On_disable();
}

void
Px4_vehicle::On_autopilot_version(
    mavlink::Message<mavlink::MESSAGE_ID::AUTOPILOT_VERSION>::Ptr ver)
{
    int maj = (ver->payload->flight_sw_version.Get() >> 24) & 0xff;
    int min = (ver->payload->flight_sw_version.Get() >> 16) & 0xff;
    int patch = (ver->payload->flight_sw_version.Get() >> 8) & 0xff;
    int type = (ver->payload->flight_sw_version.Get() >> 0) & 0xff;
    LOG("PX4 version=%d.%d.%d, type=%d", maj, min, patch, type);

    if ((ver->payload->capabilities & mavlink::MAV_PROTOCOL_CAPABILITY_MAVLINK2)
        && !mav_stream->Is_mavlink_v2()
        && !use_mavlink_2)
    {
        mav_stream->Set_mavlink_v2(true);
        LOG_INFO("Enabled MAVLINK2");
    }

    if (maj > 1 || (maj == 1 && min >= 4)) {
        set_message_interval_supported = true;
    }

    if (maj > 1 || (maj == 1 && min >= 8)) {
        set_poi_supported = true;
    }

    if (maj > 1 || (maj == 1 && min >= 9)) {
        yaw_mode_str = "MPC_YAW_MODE";
    }

    if (!protocol_version_detected) {
        protocol_version_detected = true;
        auto cmd_long_set_mode = mavlink::Pld_command_long::Create();
        (*cmd_long_set_mode)->target_system = real_system_id;
        (*cmd_long_set_mode)->target_component = 100;
        (*cmd_long_set_mode)->command = mavlink::MAV_CMD::MAV_CMD_REQUEST_CAMERA_INFORMATION;
        (*cmd_long_set_mode)->param1 = 1;
        Send_message(*cmd_long_set_mode);
        read_parameters.Enable({"SYS_AUTOSTART", "GF_ACTION", "MPC_XY_VEL_MAX"});
        Download_mission();
    }
}

void
Px4_vehicle::On_camera_information(
    mavlink::Message<mavlink::MESSAGE_ID::CAMERA_INFORMATION>::Ptr camera)
{
    // override camera trigger type
    camera_trigger_type = 0;

    camera_component_id = camera->Get_sender_component_id();
    LOG_INFO("Camera found. Component id = %d", camera_component_id);

    char camera_model_name[32];
    char camera_vendor_name[32];
    for (int i = 0; i < 31; i++) {
        camera_model_name[i] = camera->payload->model_name[i];
        camera_vendor_name[i] = camera->payload->vendor_name[i];
    }

    LOG_INFO("Camera model: %s, vendor: %s", camera_model_name, camera_vendor_name);

    int dev = (camera->payload->firmware_version.Get() >> 24) & 0xff;
    int patch = (camera->payload->firmware_version.Get() >> 16) & 0xff;
    int min = (camera->payload->firmware_version.Get() >> 8) & 0xff;
    int maj = (camera->payload->firmware_version.Get() >> 0) & 0xff;
    LOG_INFO("Camera firmware version: %d.%d.%d.%d", maj, min, patch, dev);
}

void
Px4_vehicle::On_image_captured(
    mavlink::Message<mavlink::MESSAGE_ID::CAMERA_IMAGE_CAPTURED>::Ptr message)
{
    auto p = message->payload;
    if (p->capture_result == 1) {
        std::string msg = "Captured image #" + std::to_string(static_cast<int32_t>(p->image_index));
        LOG_INFO("Captured image #%d", static_cast<int32_t>(p->image_index));
        Add_status_message(msg);

    } else {
        Add_status_message("Image capturing error");
        LOG_INFO("Image capturing error");
    }
}

void
Px4_vehicle::On_home_position(mavlink::Message<mavlink::MESSAGE_ID::HOME_POSITION>::Ptr message)
{
    auto p = message->payload;
    // cast from int to float first.
    double lat = p->latitude;
    double lon = p->longitude;
    double alt = p->altitude;
    // then fix units.
    lat = lat / 10000000 * M_PI / 180;
    lon = lon / 10000000 * M_PI / 180;
    alt = alt / 1000;

    if (    fabs(home_location.latitude - lat) > 0.00001    // ~ 6 cm
        ||  fabs(home_location.longitude - lon) > 0.00001   // max ~ 6 cm
        ||  fabs(home_location.altitude - alt) > 0.01) {
        // Home has moved.
        home_location.latitude = lat;
        home_location.longitude = lon;
        home_location.altitude = alt;
        if (Is_home_position_valid()) {
            VEHICLE_LOG_INF(*this,
                "Got home position: x=%f, y=%f, z=%f, Setting new altitude origin.",
                lat, lon, alt);
            t_home_latitude->Set_value(lat);
            t_home_longitude->Set_value(lon);
            t_home_altitude_amsl->Set_value(alt);
            Calculate_current_route_id();
            Set_altitude_origin(home_location.altitude);
        }
    }
}

void
Px4_vehicle::On_parameter(
    mavlink::Message<mavlink::MESSAGE_ID::PARAM_VALUE>::Ptr m)
{
    const auto &name = m->payload->param_id.Get_string();

    if (name == "SYS_AUTOSTART") {
        float v = m->payload->param_value.Get();
        // PX4 copies int values into float directly without conversion.
        const int32_t *model = reinterpret_cast<int32_t *>(&v);
        if (*model == MODEL_TYPHOON_H520) {
            vendor = Px4_vendor::YUNEEC;
            LOG_INFO("UAV model: Typhoon H520, vendor: Yuneec");
            Set_frame_type("yuneec_h520");
        }

        // Register vehicle with ugcs once we have frame type.
        if (!Is_registered()) {
            Register();
        }
    } else if (name == "GF_ACTION") {
        // This works because float zero is the same bitwise representation as int zero.
        t_fence_enabled->Set_value(m->payload->param_value.Get() != 0);
        Commit_to_ucs();
    } else if (name == "MPC_XY_VEL_MAX") {
        max_ground_speed = m->payload->param_value.Get();
    }
}

void
Px4_vehicle::Download_mission()
{
    if (!read_waypoints.In_progress()) {
        current_command_map.Reset();
        read_waypoints.Enable();
    }
}

void
Px4_vehicle::On_mission_item(mavlink::Pld_mission_item mi)
{
    current_command_map.Accumulate_route_id(Get_mission_item_hash(mi));
//    VEHICLE_LOG_DBG(*this, "Item %d received. mission_id=%08X", mi->seq.Get(), current_command_map.Get_route_id());
}

void
Px4_vehicle::Calculate_current_route_id()
{
    float hl;
    if (t_home_altitude_amsl->Get_value(hl)) {
        current_command_map.Set_secondary_id(hl * 10);
    }
    current_route_id = current_command_map.Get_route_id();
    VEHICLE_LOG_DBG(*this, "New mission_id=%08X", current_route_id);
    t_current_mission_id->Set_value(current_route_id);
}

void
Px4_vehicle::On_mission_downloaded(bool, std::string)
{
    Calculate_current_route_id();
    VEHICLE_LOG_DBG(*this, "Mission_downloaded. mission_id=%08X", current_route_id);
    Commit_to_ucs();
}

void
Px4_vehicle::Initialize_telemetry()
{
    if (set_message_interval_supported) {
        for (auto it : telemetry_rates)
        {
            // Send message twice to be sure.
            // TODO: Rework this to verify the actual interval used by px4.
            // Need to refactor the Send_message to include response handler.
            Set_message_interval(it.first, 1000000.0 / it.second);
            Set_message_interval(it.first, 1000000.0 / it.second);
        }
    } else {
        Mavlink_vehicle::Initialize_telemetry();
    }
}

bool
Px4_vehicle::Is_home_position_valid()
{
    return (home_location.latitude != 0) || (home_location.longitude != 0);
}

void
Px4_vehicle::Handle_vehicle_request(Vehicle_task_request::Handle request)
{
    VEHICLE_LOG_INF((*this), "Starting to handle %zu tasks...", request->actions.size());
    ASSERT(!task_upload.request);
    task_upload.Disable();
    task_upload.Enable(request);
}

void
Px4_vehicle::Handle_ucs_command(
    Ucs_request::Ptr ucs_request)
{
    if (vehicle_command.ucs_request) {
        Command_failed(ucs_request, "Previous request in progress");
        return;
    }

    if (ucs_request->request.device_commands_size() == 0) {
        Command_failed(ucs_request, "No commands found", proto::STATUS_INVALID_COMMAND);
        return;
    }

    try {
        auto &vsm_cmd = ucs_request->request.device_commands(0);

        auto cmd = Get_command(vsm_cmd.command_id());

        if (cmd == c_mission_upload || cmd == c_get_native_route) {
            VEHICLE_LOG_INF((*this), "COMMAND %s", Dump_command(vsm_cmd).c_str());
            if ((cmd == c_mission_upload) && read_waypoints.In_progress()) {
                Command_failed(ucs_request, "Mission download in progress");
                return;
            }
            Vehicle::Handle_ucs_command(ucs_request);
            return;
        }

        vehicle_command.Disable("Internal error");
        vehicle_command.ucs_request = ucs_request;
        vehicle_command.Enable();
    } catch (const std::exception& ex) {
        Command_failed(ucs_request, ex.what(), proto::STATUS_INVALID_PARAM);
    }
}

void
Px4_vehicle::Start_direct_vehicle_control()
{
    if (direct_vehicle_control == nullptr) {
        // Create rc_override message. timer will delete it when vehicle switched to other mode.
        direct_vehicle_control = mavlink::Pld_manual_control::Create();
        (*direct_vehicle_control)->target = real_system_id;

        direct_vehicle_control_timer = Timer_processor::Get_instance()->Create_timer(
            MANUAL_CONTROL_PERIOD,
            Make_callback(&Px4_vehicle::Direct_vehicle_control_timer, Shared_from_this()),
            Get_completion_ctx());
    }
    // Set larger timeout when turning on joystick mode
    // to let client more time to understand that joystick commands must be sent, now.
    direct_vehicle_control_last_received = std::chrono::steady_clock::now() + MANUAL_CONTROL_TIMEOUT;
    Set_direct_vehicle_control(0, 0, 0, 0);
    Send_direct_vehicle_control();
}

void
Px4_vehicle::Stop_direct_vehicle_control()
{
    Set_direct_vehicle_control(0, 0, 0, 0);
    Send_direct_vehicle_control();
    direct_vehicle_control = nullptr;
}

bool
Px4_vehicle::Direct_vehicle_control_timer()
{
    if (direct_vehicle_control == nullptr) {
        return false;
    }

    auto now = std::chrono::steady_clock::now();

    if (now - direct_vehicle_control_last_received > MANUAL_CONTROL_TIMEOUT) {
        // Automatically exit joystick mode if there are no control messages from ucs.
        Stop_direct_vehicle_control();
        return false;
    }

    if (now - direct_vehicle_control_last_sent < MANUAL_CONTROL_PERIOD) {
        // Do not spam radio link too much.
        return true;
    }

    Send_direct_vehicle_control();

    return true;
}

void
Px4_vehicle::Set_direct_vehicle_control(int p, int r, int t, int y)
{
    if (direct_vehicle_control) {
        (*direct_vehicle_control)->x = p;
        (*direct_vehicle_control)->y = r;
        (*direct_vehicle_control)->z = t;
        (*direct_vehicle_control)->r = y;
    }
}

void
Px4_vehicle::Send_direct_vehicle_control()
{
    if (direct_vehicle_control) {
//        LOG("Direct vehicle %d %d %d %d",
//            (*direct_vehicle_control)->x.Get(),
//            (*direct_vehicle_control)->y.Get(),
//            (*direct_vehicle_control)->z.Get(),
//            (*direct_vehicle_control)->r.Get()
//            );
        mav_stream->Send_message(
                *direct_vehicle_control,
                255,
                190,
                Mavlink_vehicle::WRITE_TIMEOUT,
                Make_timeout_callback(
                        &Mavlink_vehicle::Write_to_vehicle_timed_out,
                        Shared_from_this(),
                        mav_stream),
                Get_completion_ctx());

        direct_vehicle_control_last_sent = std::chrono::steady_clock::now();
    }
}

void
Px4_vehicle::Vehicle_command_act::Set_mode(uint8_t main_mode, uint8_t sub_mode)
{
    auto cmd_long = mavlink::Pld_command_long::Create();
    Fill_target_ids(*cmd_long);

    (*cmd_long)->command = mavlink::MAV_CMD::MAV_CMD_DO_SET_MODE;
    // Base mode
    (*cmd_long)->param1 = px4_vehicle.Get_base_mode() | mavlink::MAV_MODE_FLAG::MAV_MODE_FLAG_CUSTOM_MODE_ENABLED;
    (*cmd_long)->param2 = main_mode; // Main mode
    (*cmd_long)->param3 = sub_mode;  // Sub mode
    (*cmd_long)->confirmation = 1;
    cmd_messages.emplace_back(cmd_long);
}

void
Px4_vehicle::Vehicle_command_act::Do_reposition(
    float latitude,
    float longitude,
    float altitude,
    float heading,
    float speed)
{
    auto cmd_long = mavlink::Pld_command_long::Create();
    Fill_target_ids(*cmd_long);
    (*cmd_long)->command = mavlink::MAV_CMD::MAV_CMD_DO_REPOSITION;
    (*cmd_long)->confirmation = 0;

    // Ground speed, less than 0 (-1) for default.
    // Not supported currently (px4 1.4.4). Vehicle uses default speed.
    (*cmd_long)->param1 = speed;

    (*cmd_long)->param2 = mavlink::MAV_DO_REPOSITION_FLAGS_CHANGE_MODE;
    (*cmd_long)->param3 = 0.0f; // Reserved

    // Yaw heading, NaN for unchanged. For planes indicates loiter direction (0: clockwise, 1: counter clockwise)
    // Not supported currently (px4 1.6.2). Vehicle does not change heading.
    (*cmd_long)->param4 = heading;

    (*cmd_long)->param5 = 1E7f * 180.0f * latitude / M_PI; // Latitude (deg * 1E7)
    (*cmd_long)->param6 = 1E7f * 180.0f * longitude / M_PI; // Longitude (deg * 1E7)
    (*cmd_long)->param7 = altitude; // Altitude (meters)
    cmd_messages.emplace_back(cmd_long);
}

void
Px4_vehicle::Vehicle_command_act::Process_arm()
{
    if (!vehicle.Is_armed() && vehicle.Is_control_mode(proto::CONTROL_MODE_AUTO)) {
        Disable("Arming disabled while in AUTO mode");
        return;
    }

    auto cmd_long = mavlink::Pld_command_long::Create();
    Fill_target_ids(*cmd_long);
    (*cmd_long)->command = mavlink::MAV_CMD::MAV_CMD_COMPONENT_ARM_DISARM;
    (*cmd_long)->param1 = 1;    // arm
    (*cmd_long)->confirmation = 0;
    cmd_messages.emplace_back(cmd_long);
    Register_status_text();
}

void
Px4_vehicle::Vehicle_command_act::Process_disarm()
{
    auto cmd_long = mavlink::Pld_command_long::Create();
    Fill_target_ids(*cmd_long);
    (*cmd_long)->command = mavlink::MAV_CMD::MAV_CMD_COMPONENT_ARM_DISARM;
    (*cmd_long)->param1 = 0;    // disarm
    (*cmd_long)->confirmation = 0;
    cmd_messages.emplace_back(cmd_long);
    Register_status_text();
}

void
Px4_vehicle::Vehicle_command_act::Process_emergency_land()
{
    Process_disarm();
}

void
Px4_vehicle::Vehicle_command_act::Process_takeoff()
{
    Set_mode(static_cast<uint8_t>(Px4_main_mode::AUTO), static_cast<uint8_t>(Px4_auto_sub_mode::AUTO_TAKEOFF));
}

void
Px4_vehicle::Vehicle_command_act::Process_rth()
{
    Set_mode(static_cast<uint8_t>(Px4_main_mode::AUTO), static_cast<uint8_t>(Px4_auto_sub_mode::AUTO_RTL));
}

void
Px4_vehicle::Vehicle_command_act::Process_land()
{
    Set_mode(static_cast<uint8_t>(Px4_main_mode::AUTO), static_cast<uint8_t>(Px4_auto_sub_mode::AUTO_LAND));
}

void
Px4_vehicle::Vehicle_command_act::Process_guided()
{
    if (!px4_vehicle.is_airborne) {
        Set_mode(static_cast<uint8_t>(Px4_main_mode::AUTO), static_cast<uint8_t>(Px4_auto_sub_mode::AUTO_TAKEOFF));
    }
    Set_mode(static_cast<uint8_t>(Px4_main_mode::AUTO), static_cast<uint8_t>(Px4_auto_sub_mode::AUTO_LOITER));
}

void
Px4_vehicle::Vehicle_command_act::Process_joystick()
{
    Set_mode(static_cast<uint8_t>(Px4_main_mode::POSCTL));
    px4_vehicle.Start_direct_vehicle_control();
}

void
Px4_vehicle::Vehicle_command_act::Process_auto()
{
    auto set_current = mavlink::Pld_mission_set_current::Create();
    Fill_target_ids(*set_current);
    (*set_current)->seq = 0;
    cmd_messages.emplace_back(set_current);
    if (!px4_vehicle.is_airborne) {
        Set_mode(static_cast<uint8_t>(Px4_main_mode::AUTO), static_cast<uint8_t>(Px4_auto_sub_mode::AUTO_TAKEOFF));
    }
    Set_mode(static_cast<uint8_t>(Px4_main_mode::AUTO), static_cast<uint8_t>(Px4_auto_sub_mode::AUTO_MISSION));
}

void
Px4_vehicle::Vehicle_command_act::Process_manual()
{
    px4_vehicle.Stop_direct_vehicle_control();
    Set_mode(static_cast<uint8_t>(Px4_main_mode::POSCTL));
}

void
Px4_vehicle::Vehicle_command_act::Process_pause()
{
    px4_vehicle.Stop_direct_vehicle_control();
    Do_reposition();
}

void
Px4_vehicle::Vehicle_command_act::Process_resume()
{
    px4_vehicle.Stop_direct_vehicle_control();
    Set_mode(static_cast<uint8_t>(Px4_main_mode::AUTO), static_cast<uint8_t>(Px4_auto_sub_mode::AUTO_MISSION));
}

void
Px4_vehicle::Vehicle_command_act::Process_waypoint(const Property_list& params)
{
    if (px4_vehicle.Is_home_position_valid()) {
        float speed, alt, hdg, ao;
        double lat, lon;
        params.at("ground_speed")->Get_value(speed);
        params.at("latitude")->Get_value(lat);
        params.at("longitude")->Get_value(lon);
        params.at("altitude_amsl")->Get_value(alt);
        params.at("altitude_origin")->Get_value(ao);
        params.at("heading")->Get_value(hdg);
        if (!px4_vehicle.is_airborne) {
            Set_mode(
                static_cast<uint8_t>(Px4_main_mode::AUTO),
                static_cast<uint8_t>(Px4_auto_sub_mode::AUTO_TAKEOFF));
        }
        if (!vehicle.Is_control_mode(proto::CONTROL_MODE_CLICK_GO)) {
            Set_mode(
                static_cast<uint8_t>(Px4_main_mode::AUTO),
                static_cast<uint8_t>(Px4_auto_sub_mode::AUTO_LOITER));
        }

        if (px4_vehicle.vendor == Px4_vendor::YUNEEC) {
            VEHICLE_LOG_WRN(vehicle, "Ignoring speed setting as MPC_XY_CRUISE is not supported by Yuneec.");
        } else {
            auto param = mavlink::Pld_param_set::Create();
            Fill_target_ids(*param);
            (*param)->param_id = "MPC_XY_CRUISE";
            (*param)->param_type = mavlink::MAV_PARAM_TYPE::MAV_PARAM_TYPE_REAL32;
            (*param)->param_value = speed;
            cmd_messages.emplace_back(param);

            if (px4_vehicle.max_ground_speed < speed) {
                auto param = mavlink::Pld_param_set::Create();
                Fill_target_ids(*param);
                (*param)->param_id = "MPC_XY_VEL_MAX";
                (*param)->param_type = mavlink::MAV_PARAM_TYPE::MAV_PARAM_TYPE_REAL32;
                (*param)->param_value = speed;
                cmd_messages.emplace_back(param);
            }
        }

        Do_reposition(
            lat,
            lon,
            // Convert to vehicle global frame. TODO: rework after altitude calibration feature.
            px4_vehicle.home_location.altitude + alt - ao,
            hdg,
            speed);
    } else {
        Disable("Invalid home position");
    }
}

void
Px4_vehicle::Vehicle_command_act::Process_set_poi(const Property_list& params)
{
    auto cmd_long = mavlink::Pld_command_long::Create();
    Fill_target_ids(*cmd_long);
    bool active = false;
    params.at("active")->Get_value(active);
    if (active) {
        double latitude;
        double longitude;
        float altitude_amsl;
        params.at("altitude_amsl")->Get_value(altitude_amsl);
        params.at("latitude")->Get_value(latitude);
        params.at("longitude")->Get_value(longitude);
        (*cmd_long)->param5 = latitude * 180.0 / M_PI;
        (*cmd_long)->param6 = longitude * 180.0 / M_PI;
        (*cmd_long)->param7 = altitude_amsl;
    } else {
        (*cmd_long)->param5 = 0;
        (*cmd_long)->param6 = 0;
        (*cmd_long)->param7 = 0;
    }
    (*cmd_long)->command = mavlink::MAV_CMD::MAV_CMD_DO_SET_ROI;
    (*cmd_long)->param1 = mavlink::MAV_ROI_LOCATION;
    cmd_messages.emplace_back(cmd_long);
}

void
Px4_vehicle::Vehicle_command_act::Process_direct_payload_control(const Property_list& params)
{
    float pitch, yaw;
    params.at("pitch")->Get_value(pitch);
    params.at("yaw")->Get_value(yaw);
    //LOG("Direct payload (py) %1.3f %1.3f", pitch, yaw);

    px4_vehicle.payload_pitch += pitch * DIRECT_PAYLOAD_CONTROLLING_COEF;
    px4_vehicle.payload_yaw += yaw * DIRECT_PAYLOAD_CONTROLLING_COEF;

    if (px4_vehicle.payload_pitch > 0) {px4_vehicle.payload_pitch = 0;}
    if (px4_vehicle.payload_pitch < -90) {px4_vehicle.payload_pitch = -90;}
    if (px4_vehicle.payload_yaw > 180) {px4_vehicle.payload_yaw -= 360;}
    if (px4_vehicle.payload_yaw < -180) {px4_vehicle.payload_yaw += 360;}

    auto cmd_long = mavlink::Pld_command_long::Create();
    Fill_target_ids(*cmd_long);
    (*cmd_long)->command = mavlink::MAV_CMD::MAV_CMD_DO_MOUNT_CONTROL;
    (*cmd_long)->param1 = px4_vehicle.payload_pitch;
    (*cmd_long)->param2 = 0;
    (*cmd_long)->param3 = px4_vehicle.payload_yaw;
    (*cmd_long)->param7 = mavlink::MAV_MOUNT_MODE::MAV_MOUNT_MODE_MAVLINK_TARGETING;
    cmd_messages.emplace_back(cmd_long);
}

void
Px4_vehicle::Vehicle_command_act::Process_direct_vehicle_control(const Property_list& params)
{
    float pitch, yaw, roll, throttle;
    params.at("pitch")->Get_value(pitch);
    params.at("yaw")->Get_value(yaw);
    params.at("roll")->Get_value(roll);
    params.at("throttle")->Get_value(throttle);

//    LOG("Direct Vehicle (rpyt) %1.3f %1.3f %1.3f %1.3f",
//        roll,
//        pitch,
//        yaw,
//        throttle);

    px4_vehicle.Set_direct_vehicle_control(pitch * 1000, roll * 1000, throttle * 1000, yaw * 1000);
    px4_vehicle.direct_vehicle_control_last_received = std::chrono::steady_clock::now();
}

bool
Px4_vehicle::Vehicle_command_act::Try()
{
    if (!remaining_attempts--) {
        VEHICLE_LOG_WRN(vehicle, "Vehicle_command all attempts failed.");
        Disable("Vehicle_command all attempts failed.");
        return false;
    }

    if (cmd_messages.size()) {
        auto cmd = cmd_messages.front();
        Send_message(*(cmd_messages.front()));
        Schedule_timer();
        VEHICLE_LOG_DBG(vehicle, "Sending to vehicle: %s", (*(cmd_messages.front())).Dump().c_str());
    } else {
        // Command list is empty, nothing to do.
        Disable("Command list empty");
    }
    return false;
}

void
Px4_vehicle::Vehicle_command_act::Send_next_command()
{
    cmd_messages.pop_front();
    if (cmd_messages.size()) {
        // send next command in chain.
        remaining_attempts = try_count;
        Send_message(*(cmd_messages.front()));
        Schedule_timer();
        VEHICLE_LOG_DBG(vehicle, "Sending to vehicle: %s", (*(cmd_messages.front())).Dump().c_str());
    } else {
        // command chain succeeded.
        Disable_success();
    }
}

void
Px4_vehicle::Vehicle_command_act::On_mission_current(
    mavlink::Message<mavlink::MESSAGE_ID::MISSION_CURRENT>::Ptr message)
{
    if (cmd_messages.size()) {
        // we are waiting for response.
        auto cmd = cmd_messages.front();
        if (    cmd->Get_id() == mavlink::MESSAGE_ID::MISSION_SET_CURRENT
            &&  message->payload->seq == (*std::static_pointer_cast<mavlink::Pld_mission_set_current>(cmd))->seq)
        {
            Send_next_command();
        }
    }
}

void
Px4_vehicle::Vehicle_command_act::On_command_ack(
        mavlink::Message<mavlink::MESSAGE_ID::COMMAND_ACK>::Ptr message)
{
    VEHICLE_LOG_DBG(vehicle, "COMMAND_ACK for command %d, res=%d",
            message->payload->command.Get(), message->payload->result.Get());

    if (cmd_messages.size()) {
        // we are waiting for response.
        auto cmd = cmd_messages.front();
        int command_id = cmd->Get_id();
        if (command_id == mavlink::MESSAGE_ID::COMMAND_LONG) {
            command_id = (*std::static_pointer_cast<mavlink::Pld_command_long>(cmd))->command.Get();
        }
        if (message->payload->command.Get() == command_id) {
            // This is a response to our command.
            if (message->payload->result == mavlink::MAV_RESULT::MAV_RESULT_ACCEPTED) {
                Send_next_command();
            } else if (px4_vehicle.vendor == Px4_vendor::YUNEEC
                       && message->payload->command.Get() == mavlink::MAV_CMD_SET_CAMERA_MODE
                       && message->payload->result == mavlink::MAV_RESULT::MAV_RESULT_IN_PROGRESS) {
                // Yuneec payloads return TWO ack on set_camera_mode command.
                // first is IN PROGRESS and second is ACCEPTED.
                // so skip MAV_RESULT_IN_PROGRESS for this case.
                //
                // maybe Yuneec will fix it in future versions.
                VEHICLE_LOG_DBG(vehicle, "YUNEEC SET_CAMERA_MODE in progress");
            } else {
                auto p = message->payload->result.Get();
                Disable("Result: " + std::to_string(p) + " (" + Mav_result_to_string(p).c_str() + ")");
            }
        }
    }
}

void
Px4_vehicle::Vehicle_command_act::On_mission_ack(
        mavlink::Message<mavlink::MESSAGE_ID::MISSION_ACK>::Ptr message)
{
    VEHICLE_LOG_INF(vehicle, "MISSION_ACK, result %d",
            message->payload->type.Get());

    if (cmd_messages.size()) {
        if (message->payload->type == mavlink::MAV_MISSION_RESULT::MAV_MISSION_ACCEPTED) {
            Send_next_command();
        } else {
            auto p = message->payload->type.Get();
            Disable("MISSION_ACK result: " + std::to_string(p) + " (" + Mav_mission_result_to_string(p).c_str() + ")");
        }
    }
}

void
Px4_vehicle::Vehicle_command_act::On_param_value(
        mavlink::Message<mavlink::MESSAGE_ID::PARAM_VALUE>::Ptr message)
{
    VEHICLE_LOG_INF(vehicle, "PARAM_VALUE, %s", message->payload.Dump().c_str());

    if (cmd_messages.size()) {
        std::string param_name;
        // we are waiting for response.
        auto cmd = cmd_messages.front();
        switch (cmd->Get_id()) {
        case mavlink::MESSAGE_ID::PARAM_REQUEST_READ:
            param_name = (*std::static_pointer_cast<mavlink::Pld_param_request_read>(cmd))->param_id.Get_string();
            if (message->payload->param_id.Get_string() == param_name) {
                Send_next_command();
            }
            break;
        case mavlink::MESSAGE_ID::PARAM_SET:
            param_name = (*std::static_pointer_cast<mavlink::Pld_param_set>(cmd))->param_id.Get_string();
            if (message->payload->param_id.Get_string() == param_name) {
                auto param_value = (*std::static_pointer_cast<mavlink::Pld_param_set>(cmd))->param_value.Get();
                if (message->payload->param_value.Get() == param_value) {
                    Send_next_command();
                } else {
                    Disable("PARAM_SET failed");
                }
            }
            break;
        }
    }
}

void
Px4_vehicle::Vehicle_command_act::On_status_text(
        mavlink::Message<mavlink::MESSAGE_ID::STATUSTEXT>::Ptr)
{
    /* Assumed command execution started, so wait longer. */
    if (current_timeout < extended_retry_timeout) {
        current_timeout = extended_retry_timeout;
        VEHICLE_LOG_DBG(vehicle, "Command execution detected, "
                "now waiting longer for a command to finish...");
        /* Start a new longer timer. */
        Schedule_timer();
    }
}

void
Px4_vehicle::Vehicle_command_act::Enable()
{
    Register_mavlink_handler<mavlink::MESSAGE_ID::COMMAND_ACK>(
        &Vehicle_command_act::On_command_ack,
        this,
        Mavlink_demuxer::COMPONENT_ID_ANY);

    Register_mavlink_handler<mavlink::MESSAGE_ID::MISSION_ACK>(
        &Vehicle_command_act::On_mission_ack,
        this,
        Mavlink_demuxer::COMPONENT_ID_ANY);

    Register_mavlink_handler<mavlink::MESSAGE_ID::MISSION_CURRENT>(
        &Vehicle_command_act::On_mission_current,
        this,
        Mavlink_demuxer::COMPONENT_ID_ANY);

    Register_mavlink_handler<mavlink::MESSAGE_ID::PARAM_VALUE>(
        &Vehicle_command_act::On_param_value,
        this,
        Mavlink_demuxer::COMPONENT_ID_ANY);

    remaining_attempts = try_count;
    current_timeout = retry_timeout;

    cmd_messages.clear();

    for (int c = 0; ucs_request && c < ucs_request->request.device_commands_size(); c++) {
        auto &vsm_cmd = ucs_request->request.device_commands(c);
        auto cmd = vehicle.Get_command(vsm_cmd.command_id());

        if (cmd != vehicle.c_direct_vehicle_control && cmd != vehicle.c_direct_payload_control) {
            // Do not spam log with direct control messages.
            VEHICLE_LOG_INF(vehicle, "COMMAND %s", vehicle.Dump_command(vsm_cmd).c_str());
        }

        auto params = cmd->Build_parameter_list(vsm_cmd);
        if (cmd == vehicle.c_emergency_land) {
            Process_emergency_land();
        } else if (cmd == vehicle.c_arm) {
            Process_arm();
        } else if (cmd == vehicle.c_disarm) {
            Process_disarm();
        } else if (cmd == vehicle.c_takeoff_command) {
            Process_takeoff();
        } else if (cmd == vehicle.c_resume) {
            Process_resume();
        } else if (cmd == vehicle.c_pause) {
            Process_pause();
        } else if (cmd == vehicle.c_auto) {
            Process_auto();
        } else if (cmd == vehicle.c_manual) {
            Process_manual();
        } else if (cmd == vehicle.c_rth) {
            Process_rth();
        } else if (cmd == vehicle.c_land_command) {
            Process_land();
        } else if (cmd == vehicle.c_waypoint) {
            Process_waypoint(params);
        } else if (cmd == vehicle.c_set_poi) {
            Process_set_poi(params);
        } else if (cmd == vehicle.c_guided) {
            Process_guided();
        } else if (cmd == vehicle.c_direct_payload_control) {
            Process_direct_payload_control(params);
        } else if (cmd == vehicle.c_direct_vehicle_control) {
            Process_direct_vehicle_control(params);
        } else if (cmd == vehicle.c_joystick) {
            Process_joystick();
        } else {
            Disable("Unsupported command");
        }
    }
    command_count = cmd_messages.size();
    Try();
}

void
Px4_vehicle::Vehicle_command_act::On_disable()
{
    Unregister_status_text();

    if (timer) {
        timer->Cancel();
        timer = nullptr;
    }
}

void
Px4_vehicle::Vehicle_command_act::Schedule_timer()
{
    if (timer) {
        timer->Cancel();
    }
    timer = Timer_processor::Get_instance()->Create_timer(
                current_timeout,
                Make_callback(&Vehicle_command_act::Try, this),
                vehicle.Get_completion_ctx());
}

void // ?
Px4_vehicle::Vehicle_command_act::Register_status_text()
{
    vehicle.statistics.statustext_handler =
            Mavlink_vehicle::Statistics::Make_statustext_handler(
                    &Px4_vehicle::Vehicle_command_act::On_status_text,
                    this);
}

void // ?
Px4_vehicle::Vehicle_command_act::Unregister_status_text()
{
    vehicle.statistics.statustext_handler =
            Mavlink_vehicle::Statistics::Statustext_handler();
}

void
Px4_vehicle::Task_upload::Enable(Vehicle_task_request::Handle request)
{
    // Clean state.
    prepared_actions.clear();
    task_attributes.clear();
    current_mission_poi.Disengage();
    current_mission_heading.Disengage();
    first_mission_poi_set = false;
    restart_mission_poi = false;
    current_heading = 0.0;
    current_speed = -1;
    heading_to_this_wp = 0.0;
    camera_series_by_dist_active = false;
    camera_series_by_dist_active_in_wp = false;
    camera_series_by_time_active = false;
    camera_series_by_time_active_in_wp = false;
    max_mission_speed = 0;

    if (px4_vehicle.vendor == Px4_vendor::YUNEEC) {
        if (request->attributes->rc_loss != Task_attributes_action::DO_NOT_CHANGE ||
            request->attributes->gnss_loss != Task_attributes_action::DO_NOT_CHANGE ||
            request->attributes->low_battery != Task_attributes_action::DO_NOT_CHANGE)
        {
            request.Fail("Failsafe actions not supported by Yuneec");
            Disable();
            return;
        }
    }

    if (!request->return_native_route)
    {
        // HL altitude becomes altitude origin.
        // Need to set at the very beginning as it is used to specify safe_altitude, too.
        float hl;
        if (vehicle.t_home_altitude_amsl->Get_value(hl)) {
            vehicle.Add_status_message("Using current HL altitude as altitude origin for the route.");
            VEHICLE_LOG_WRN(
                vehicle,
                "Using current HL altitude %f m as altitude origin for route.",
                hl);
            request->Set_takeoff_altitude(hl);
        } else {
            // Older PX4 firmware does not report HL.
            vehicle.Add_status_message("Cannot determine Home Location. Using altitude origin from route.");
            VEHICLE_LOG_WRN(
                vehicle,
                "Cannot determine Home Location. Using altitude origin %f m from route.",
                request->Get_takeoff_altitude());
        }
    }

    this->request = request;

    Filter_actions();

    if (max_mission_speed > MAX_COPTER_SPEED) {
        VEHICLE_LOG_WRN(vehicle, "Max speed used in mission %f exceeds the max allowed %f m/s.",
            max_mission_speed,
            Px4_vehicle::MAX_COPTER_SPEED);
        max_mission_speed = MAX_COPTER_SPEED;
    }

    if (request->return_native_route) {
        Prepare_task();
        std::string mission = Generate_wpl(prepared_actions, request->use_crlf_in_native_route);
        request->ucs_response->mutable_device_response()->set_status(mission);
        request.Succeed();
        Disable();
        return;
    }

    Prepare_task_attributes();

    vehicle.write_parameters.Disable();
    vehicle.write_parameters.Set_next_action(
            Write_parameters::Make_next_action(
                    &Task_upload::Task_atributes_uploaded,
                    this));
    vehicle.write_parameters.Enable(task_attributes);
}

void
Px4_vehicle::Task_upload::Task_atributes_uploaded(bool success, std::string error_msg)
{
    if (!success) {
        if (error_msg.size()) {
            request.Fail(error_msg);
        } else {
            request.Fail("Task attributes upload failed");
        }
        Disable();
        return;
    }

    Prepare_task();
    vehicle.mission_upload.Disable();
    vehicle.mission_upload.mission_items = std::move(prepared_actions);
    vehicle.mission_upload.Set_next_action(
            Activity::Make_next_action(
                    &Task_upload::Mission_uploaded,
                    this));
    vehicle.mission_upload.Enable();
}

void
Px4_vehicle::Task_upload::Mission_uploaded(bool success, std::string error_msg)
{
    if (!success) {
        if (error_msg.size()) {
            request.Fail(error_msg);
        } else {
            request.Fail("Route upload failed");
        }
        Disable();
        return;
    }


    px4_vehicle.Calculate_current_route_id();

    LOG("Uploaded mission_id=%08X", px4_vehicle.current_route_id);
    vehicle.current_command_map.Fill_command_mapping_response(request->ucs_response);

    /* Everything is OK. */
    request.Succeed();
    Disable();
}

void
Px4_vehicle::Task_upload::Fill_mavlink_mission_item_coords(
        mavlink::Pld_mission_item& msg,
        const Geodetic_tuple& tuple, double heading)
{
    msg->x = (tuple.latitude * 180.0) / M_PI;
    msg->y = (tuple.longitude * 180.0) / M_PI;
    /* Fixup absolute altitude - make them relative to
     * take-off altitude.
     */
    msg->z = tuple.altitude - request->Get_takeoff_altitude();
    msg->param4 = (heading * 180.0) / M_PI;
}

void
Px4_vehicle::Task_upload::Fill_mavlink_mission_item_common(
        mavlink::Pld_mission_item& msg)
{
    ASSERT(vehicle.real_system_id != Mavlink_demuxer::SYSTEM_ID_ANY);

    Fill_target_ids(msg);
    msg->seq = prepared_actions.size();

    vehicle.current_command_map.Accumulate_route_id(Get_mission_item_hash(msg));
    vehicle.current_command_map.Add_command_mapping(msg->seq);

    switch (msg->command) {
    case mavlink::MAV_CMD_DO_CHANGE_SPEED:
    case mavlink::MAV_CMD_DO_SET_SERVO:
    case mavlink::MAV_CMD_DO_DIGICAM_CONTROL:
    case mavlink::MAV_CMD_DO_MOUNT_CONFIGURE:
    case mavlink::MAV_CMD_DO_MOUNT_CONTROL:
    case mavlink::MAV_CMD_IMAGE_START_CAPTURE:
    case mavlink::MAV_CMD_IMAGE_STOP_CAPTURE:
    case mavlink::MAV_CMD_VIDEO_START_CAPTURE:
    case mavlink::MAV_CMD_VIDEO_STOP_CAPTURE:
    case mavlink::MAV_CMD_SET_CAMERA_MODE:
    case mavlink::MAV_CMD_DO_SET_CAM_TRIGG_DIST:
    case mavlink::MAV_CMD_DO_VTOL_TRANSITION:
    case mavlink::MAV_CMD_DO_SET_ROI_NONE:
        msg->frame = mavlink::MAV_FRAME::MAV_FRAME_MISSION;
        break;
    default:
        msg->frame = mavlink::MAV_FRAME::MAV_FRAME_GLOBAL_RELATIVE_ALT;
        break;
    }
    msg->autocontinue = 1;
}

void
Px4_vehicle::Task_upload::On_disable()
{
    request.Fail();
    vehicle.write_parameters.Disable();
    vehicle.mission_upload.Disable();
    prepared_actions.clear();
    task_attributes.clear();
    current_mission_poi.Disengage();
    current_mission_heading.Disengage();
    current_camera_mode.Disengage();
    last_move_action = nullptr;
    takeoff_action = nullptr;
    first_mission_poi_set = false;
    restart_mission_poi = false;
    current_heading = 0;
}

void
Px4_vehicle::Task_upload::Filter_actions()
{
    switch (vehicle.Get_vehicle_type()) {
    case proto::VEHICLE_TYPE_HELICOPTER:
    case proto::VEHICLE_TYPE_MULTICOPTER:
    case proto::VEHICLE_TYPE_VTOL:
        Filter_copter_actions();
        return;
    case proto::VEHICLE_TYPE_FIXED_WING:
        Filter_plane_actions();
        return;
    case proto::VEHICLE_TYPE_GROUND:
        Filter_rover_actions();
        return;
    }
    VSM_EXCEPTION(Internal_error_exception, "Unhandled PX4 vehicle type %d.", vehicle.Get_vehicle_type());
}

void
Px4_vehicle::Task_upload::Filter_copter_actions()
{
    for (auto iter = request->actions.begin(); iter != request->actions.end();) {
        switch ((*iter)->Get_type()) {
        case Action::Type::CHANGE_SPEED:
        {
            auto csa = (*iter)->Get_action<Action::Type::CHANGE_SPEED>();
            if (csa->speed > max_mission_speed) {
                max_mission_speed = csa->speed;
            }
        }
        /* no break */
        default:
            iter++;
        continue;
        }
        iter = request->actions.erase(iter);
    }
}

void
Px4_vehicle::Task_upload::Filter_plane_actions()
{
    for (auto iter = request->actions.begin(); iter != request->actions.end();) {
        switch ((*iter)->Get_type()) {
        case Action::Type::CAMERA_CONTROL:
            VEHICLE_LOG_WRN(vehicle, "CAMERA_CONTROL action ignored.");
            break;
        case Action::Type::CAMERA_TRIGGER:
            VEHICLE_LOG_WRN(vehicle, "CAMERA_TRIGGER action ignored.");
            break;
        case Action::Type::PANORAMA:
            VEHICLE_LOG_WRN(vehicle, "PANORAMA action ignored.");
            break;
        case Action::Type::POI:
            VEHICLE_LOG_WRN(vehicle, "POI action ignored.");
            break;
        case Action::Type::HEADING:
            VEHICLE_LOG_WRN(vehicle, "HEADING action ignored.");
            break;
        default:
            iter++;
            continue;
        }
        iter = request->actions.erase(iter);
    }
}

void
Px4_vehicle::Task_upload::Filter_rover_actions()
{
    for (auto iter = request->actions.begin(); iter != request->actions.end();) {
        switch ((*iter)->Get_type()) {
        case Action::Type::CAMERA_CONTROL:
            VEHICLE_LOG_WRN(vehicle, "CAMERA_CONTROL action ignored.");
            break;
        case Action::Type::CAMERA_TRIGGER:
            VEHICLE_LOG_WRN(vehicle, "CAMERA_TRIGGER action ignored.");
            break;
        case Action::Type::PANORAMA:
            VEHICLE_LOG_WRN(vehicle, "PANORAMA action ignored.");
            break;
        case Action::Type::POI:
            VEHICLE_LOG_WRN(vehicle, "POI action ignored.");
            break;
        case Action::Type::HEADING:
            VEHICLE_LOG_WRN(vehicle, "HEADING action ignored.");
            break;
        default:
            iter++;
            continue;
        }
        iter = request->actions.erase(iter);
    }
}

void
Px4_vehicle::Task_upload::Filter_other_actions()
{
    /* Only move is supported. Safe. */
    for (auto iter = request->actions.begin(); iter != request->actions.end();) {
        switch ((*iter)->Get_type()) {
        case Action::Type::MOVE:
            iter++;
            continue;
        default:
            VEHICLE_LOG_WRN(vehicle, "Action type %d ignored.", static_cast<int>((*iter)->Get_type()));
            break;
        }
        iter = request->actions.erase(iter);
    }
}

void
Px4_vehicle::Task_upload::Prepare_task()
{
    prepared_actions.clear();
    vehicle.current_command_map.Reset();
    last_move_action = nullptr;
    takeoff_action = nullptr;
    for (auto& iter : request->actions) {
        vehicle.current_command_map.Set_current_command(iter->command_id);
        Prepare_action(iter);
    }
}

void
Px4_vehicle::Task_upload::Prepare_task_attributes()
{
    task_attributes.clear();
    if (request->attributes == nullptr) {
        return;
    }
    switch (vehicle.Get_vehicle_type()) {
    case proto::VEHICLE_TYPE_HELICOPTER:
    case proto::VEHICLE_TYPE_MULTICOPTER:
    case proto::VEHICLE_TYPE_VTOL:
        Prepare_copter_task_attributes();
        return;
    case proto::VEHICLE_TYPE_FIXED_WING:
        Prepare_plane_task_attributes();
        return;
    case proto::VEHICLE_TYPE_GROUND:
        Prepare_rover_task_attributes();
        return;
    }
    VSM_EXCEPTION(Internal_error_exception, "Unhandled PX4 vehicle type %d", vehicle.Get_vehicle_type());
}

void // ?
Px4_vehicle::Task_upload::Prepare_copter_task_attributes()
{
    using Emerg = Task_attributes_action::Emergency_action;

    /* Battery failsafe. */
    if (request->attributes->low_battery != Emerg::DO_NOT_CHANGE) {
        int low_batt;
        switch (request->attributes->low_battery) {
        case Emerg::GO_HOME:
            low_batt = BATT_FS_RTH;
            break;
        case Emerg::LAND:
            low_batt = BATT_FS_LAND;
            break;
        case Emerg::CONTINUE:
            low_batt = BATT_FS_WARNING;
            break;
        default:
            /* There is no support for such behavior. Override with gohome. */
            VEHICLE_LOG_WRN(vehicle, "Unsupported FS action %d. using gohome", request->attributes->low_battery);
            low_batt = BATT_FS_RTH;
            break;
        }
        task_attributes.Append_int_px4("COM_LOW_BAT_ACT", low_batt);
    }

    /* RC loss failsafe. */
    if (request->attributes->rc_loss != Emerg::DO_NOT_CHANGE) {
        int rc_loss;
        switch (request->attributes->rc_loss) {
        case Emerg::WAIT:
            rc_loss = RC_FS_LOITER;
            break;
        case Emerg::LAND:
            rc_loss = RC_FS_LAND;
            break;
        case Emerg::GO_HOME:
            rc_loss = RC_FS_RTH;
            break;
        default:
            rc_loss = RC_FS_DISABLED;
            break;
        }
        task_attributes.Append_int_px4("NAV_RCL_ACT", rc_loss);
    }

    if (std::isnan(request->attributes->safe_altitude)) {
        VEHICLE_LOG_INF(vehicle, "safe_altitude not specified");
    } else {
        int16_t safe_alt = (request->attributes->safe_altitude - request->Get_takeoff_altitude());
        if (safe_alt < 1) {
            // Avoid landing.
            VEHICLE_LOG_WRN(vehicle, "Forcing safe altitude to 1m");
            safe_alt = 1;
        }

        /* RTL altitude. */
        task_attributes.Append_float("RTL_RETURN_ALT", safe_alt);

        // Do not set RTL_DESCEND_ALT to safe_alt because it makes vehicle
        // to descend at landing speed which is much slower than descent speed.
        // (YUNEEC doesn't allow to change this parameter at all)
    }

    // YUNEEC doesn't allow to change this parameters
    if (px4_vehicle.vendor != Px4_vendor::YUNEEC) {
        // Maximum speed in mission
        if (px4_vehicle.max_ground_speed < max_mission_speed) {
            task_attributes.Append_float("MPC_XY_VEL_MAX", max_mission_speed);
        }

        // Do not modify MIS_YAWMODE if autoheading is not set.
        if (px4_vehicle.autoheading) {
            // Set yaw mode to WP-defined
            task_attributes.Append_int_px4(px4_vehicle.yaw_mode_str, YAWMODE_WP_DEFINED);
        }
    }
}

void // ?
Px4_vehicle::Task_upload::Prepare_plane_task_attributes()
{
    // Ardupilot VSM parses the request->parameters here
    // and sets respective parameters on the autopilot.
    // TODO: implement once we support PX4 planes.
}

void // ?
Px4_vehicle::Task_upload::Prepare_rover_task_attributes()
{
    /* Add rover specific task attributes */
}

void
Px4_vehicle::Task_upload::Prepare_action(Action::Ptr action)
{
    switch (action->Get_type()) {
    case Action::Type::MOVE:
        Prepare_move(action);
        return;
    case Action::Type::WAIT:
        Prepare_wait(action);
        return;
    case Action::Type::PAYLOAD_STEERING: // ?
        // Prepare_payload_steering(action);
        return;
    case Action::Type::TAKEOFF:
        Prepare_takeoff(action);
        return;
    case Action::Type::LANDING:
        Prepare_landing(action);
        return;
    case Action::Type::CHANGE_SPEED:
        Prepare_change_speed(action);
        return;
    case Action::Type::SET_HOME:
        // Setting HL not supported for PX4. It always resets HL to current position on ARM.
        return;
    case Action::Type::POI:
        if (px4_vehicle.set_poi_supported) {
            Prepare_POI(action);
        } else {
            VEHICLE_LOG_ERR(vehicle, "Ignoring set_poi. Not supported in PX4 version < 1.8");
        }
        return;
    case Action::Type::HEADING:
        Prepare_heading(action);
        return;
    case Action::Type::PANORAMA: // ?
        // Prepare_panorama(action);
        return;
    case Action::Type::CAMERA_CONTROL:
        Prepare_camera_control(action);
        return;
    case Action::Type::CAMERA_TRIGGER:
        Prepare_camera_trigger(action);
        return;
    case Action::Type::CAMERA_SERIES_BY_TIME:
        Prepare_camera_series_by_time(action);
        return;
    case Action::Type::CAMERA_SERIES_BY_DISTANCE: // ?
        Prepare_camera_series_by_distance(action);
        return;
    case Action::Type::VTOL_TRANSITION:
        Prepare_vtol_transition(action);
        return;
    default:
        VEHICLE_LOG_ERR(vehicle, "action %s not supported.", action->Get_name().c_str());
        break;
    }
}

void
Px4_vehicle::Task_upload::Add_mission_item(mavlink::Pld_mission_item::Ptr mi)
{
    Fill_mavlink_mission_item_common(*mi);
    prepared_actions.push_back(mi);
}

void
Px4_vehicle::Task_upload::Prepare_move(Action::Ptr& action)
{
    /* Turn off camera series if active. */
    if (!camera_series_by_dist_active_in_wp) {
        if (camera_series_by_dist_active) {
            camera_series_by_dist_active = false;
            mavlink::Pld_mission_item::Ptr mi = mavlink::Pld_mission_item::Create();
            (*mi)->command = mavlink::MAV_CMD::MAV_CMD_DO_SET_CAM_TRIGG_DIST;
            Add_mission_item(mi);
        }
    }
    if (!camera_series_by_time_active_in_wp) {
        if (camera_series_by_time_active) {
            camera_series_by_time_active = false;
            mavlink::Pld_mission_item::Ptr mi = mavlink::Pld_mission_item::Create();
            if (px4_vehicle.camera_trigger_type == 1) {
                (*mi)->command = mavlink::MAV_CMD::MAV_CMD_DO_REPEAT_SERVO;
                (*mi)->param1 = px4_vehicle.camera_servo_idx;
            } else {
                (*mi)->target_system = px4_vehicle.real_system_id;
                (*mi)->target_component = px4_vehicle.camera_component_id;
                (*mi)->command = mavlink::MAV_CMD::MAV_CMD_IMAGE_STOP_CAPTURE;
            }
            Add_mission_item(mi);
        }
    }
    camera_series_by_dist_active_in_wp = false;
    camera_series_by_time_active_in_wp = false;

    auto to = action->Get_action<Action::Type::MOVE>();

    if (last_move_action || takeoff_action) {
        float calculated_heading;
        if (last_move_action) {
            auto from = last_move_action->Get_action<Action::Type::MOVE>();
            calculated_heading = from->position.Bearing(to->position);
        } else {
            auto from = takeoff_action->Get_action<Action::Type::TAKEOFF>();
            calculated_heading = from->position.Bearing(to->position);
        }
        // Handle several waypoints at the same coords.
        if (!std::isnan(calculated_heading)) {
            calculated_heading = Normalize_angle_0_2pi(calculated_heading);
            heading_to_this_wp = calculated_heading;
        } else {
            // Use previously calculated heading_to_this_wp.
        }
    }

    if (current_mission_poi) {
        if (!first_mission_poi_set && (px4_vehicle.auto_generate_mission_poi || restart_mission_poi)) {
            // Add automatic POI on each consecutive WP.
            LOG("Set AutoPOI");
            Add_mission_item(Build_roi_mission_item(*current_mission_poi));
        }
    } else {
        if (current_mission_heading) {
            // Set current heading as yaw angle.
            current_heading = *current_mission_heading;
        } else {
            // Set heading to next waypoint as yaw angle.
            current_heading = heading_to_this_wp;
        }
        if ((last_move_action || takeoff_action) && vehicle.Is_copter()) {
            // Autoheading is copter specific.
            if (px4_vehicle.autoheading) {
                LOG("Set Autoheading to %f", current_heading);
                to->heading = current_heading;
            } else {
                to->heading = NAN;
            }
        }
    }

    auto mi = Build_wp_mission_item(action);
    if (last_move_action == nullptr) {
        // This is the first action. Mark it as current.
        (*mi)->current = 1;
    }
    Add_mission_item(mi);
    last_move_action = action;

    // restart_mission_poi = false;
    // first_mission_poi_set = false;
    current_mission_heading.Disengage();
}

void // ?
Px4_vehicle::Task_upload::Prepare_wait(Action::Ptr& action)
{
    /* Create additional waypoint on the current position to wait. */
    if (last_move_action) {
        auto wp = Build_wp_mission_item(last_move_action);
        if (!current_mission_poi && px4_vehicle.autoheading) {
            (*wp)->param4 = (Normalize_angle_0_2pi(current_heading) * 180.0) / M_PI;
        }
        first_mission_poi_set = false;
        restart_mission_poi = true;
        Wait_action::Ptr wa = action->Get_action<Action::Type::WAIT>();
        (*wp)->param1 = wa->wait_time;
        Add_mission_item(wp);
    } else {
        VEHICLE_LOG_WRN(vehicle, "No move action before wait action, ignored.");
    }
}

void // ?
Px4_vehicle::Task_upload::Prepare_payload_steering(Action::Ptr&)
{
    ASSERT(false); /* Not implemented yet */
}

void
Px4_vehicle::Task_upload::Prepare_takeoff(Action::Ptr& action)
{
    auto takeoff = action->Get_action<Action::Type::TAKEOFF>();
    mavlink::Pld_mission_item::Ptr mi = mavlink::Pld_mission_item::Create();
    if (vehicle.Is_vehicle_type(proto::VEHICLE_TYPE_VTOL)) {
        (*mi)->command = mavlink::MAV_CMD::MAV_CMD_NAV_VTOL_TAKEOFF;
    } else {
        (*mi)->command = mavlink::MAV_CMD::MAV_CMD_NAV_TAKEOFF;
    }
    (*mi)->param1 = 0; /* No data for pitch. */
    Fill_mavlink_mission_item_coords(*mi, takeoff->position.Get_geodetic(), takeoff->heading);
    Add_mission_item(mi);
    takeoff_action = action;
}

void
Px4_vehicle::Task_upload::Prepare_landing(Action::Ptr& action)
{
    auto land = action->Get_action<Action::Type::LANDING>();

    mavlink::Pld_mission_item::Ptr mi = mavlink::Pld_mission_item::Create();
    if (vehicle.Is_vehicle_type(proto::VEHICLE_TYPE_VTOL)) {
        (*mi)->command = mavlink::MAV_CMD::MAV_CMD_NAV_VTOL_LAND;
    } else {
        (*mi)->command = mavlink::MAV_CMD::MAV_CMD_NAV_LAND;
    }
    Fill_mavlink_mission_item_coords(*mi, land->position.Get_geodetic(), land->heading);
    Add_mission_item(mi);

    /* Don't duplicate waypoint if last action is land. */
    last_move_action = nullptr;
    takeoff_action = nullptr;
}

void
Px4_vehicle::Task_upload::Prepare_vtol_transition(Action::Ptr& action)
{
    if (vehicle.Is_vehicle_type(proto::VEHICLE_TYPE_VTOL)) {
        auto a = action->Get_action<Action::Type::VTOL_TRANSITION>();
        mavlink::Pld_mission_item::Ptr mi = mavlink::Pld_mission_item::Create();
        (*mi)->command = mavlink::MAV_CMD::MAV_CMD_DO_VTOL_TRANSITION;
        switch (a->mode) {
        case Vtol_transition_action::FIXED:
            (*mi)->param1 = mavlink::MAV_VTOL_STATE_FW;
            break;
        case Vtol_transition_action::VTOL:
            (*mi)->param1 = mavlink::MAV_VTOL_STATE_MC;
            break;
        }
        Add_mission_item(mi);
    } else {
        VEHICLE_LOG_WRN(vehicle, "VTOL transition not supported by vehicle. Ignored.");
    }
}

void
Px4_vehicle::Task_upload::Prepare_change_speed(Action::Ptr& action)
{
    Change_speed_action::Ptr la = action->Get_action<Action::Type::CHANGE_SPEED>();
    if (fabs(current_speed - la->speed) < CHANGE_SPEED_TRESHOLD) {
        // Do not generate change_speed if the change is too small.
        return;
    }
    current_speed = la->speed;

    mavlink::Pld_mission_item::Ptr mi = mavlink::Pld_mission_item::Create();
    (*mi)->command = mavlink::MAV_CMD::MAV_CMD_DO_CHANGE_SPEED;
    (*mi)->param1 = 1; /* Ground Speed */
    (*mi)->param2 = la->speed;
    (*mi)->param3 = -1; /* Throttle no change. */
    Add_mission_item(mi);
}

void
Px4_vehicle::Task_upload::Prepare_POI(Action::Ptr& action)
{
    Poi_action::Ptr pa = action->Get_action<Action::Type::POI>();
    mavlink::Pld_mission_item::Ptr mi;
    if (pa->active) {
        // Set up POI for succeeding waypoints.
        current_mission_poi = pa->position.Get_geodetic();
        mi = Build_roi_mission_item(*current_mission_poi);
        first_mission_poi_set = true;
    } else {
        // Reset POI. Generate next WPs as heading from now on.
        mi = mavlink::Pld_mission_item::Create();
        (*mi)->command = mavlink::MAV_CMD::MAV_CMD_DO_SET_ROI_NONE;
        current_mission_poi.Disengage();
    }
    Add_mission_item(mi);
}

void
Px4_vehicle::Task_upload::Prepare_heading(Action::Ptr& action)
{
    Heading_action::Ptr ha = action->Get_action<Action::Type::HEADING>();
    // Save heading for eventual WAIT action.
    current_heading = ha->heading;
    current_mission_heading = ha->heading;
    // Heading action terminates current POI.
    restart_mission_poi = true;

    /* Create additional waypoint on the current position for new heading. */
    if (last_move_action) {
        auto wp = Build_wp_mission_item(last_move_action);
        if (!current_mission_poi) {
            (*wp)->param4 = (Normalize_angle_0_2pi(current_heading) * 180.0) / M_PI;
        }
        first_mission_poi_set = false;
        restart_mission_poi = true;
        Add_mission_item(wp);
    }
}

void
Px4_vehicle::Task_upload::Prepare_camera_series_by_distance(Action::Ptr& action)
{
    Camera_series_by_distance_action::Ptr a =
        action->Get_action<Action::Type::CAMERA_SERIES_BY_DISTANCE>();
    mavlink::Pld_mission_item::Ptr mi = mavlink::Pld_mission_item::Create();
    (*mi)->command = mavlink::MAV_CMD::MAV_CMD_DO_SET_CAM_TRIGG_DIST;
    (*mi)->param1 = a->interval;
    Add_mission_item(mi);
    camera_series_by_dist_active = true;
    camera_series_by_dist_active_in_wp = true;
}

void
Px4_vehicle::Task_upload::Prepare_camera_mode(mavlink::CAMERA_MODE mode)
{
    int new_camera_mode = static_cast<int>(mode);
    if (!current_camera_mode || new_camera_mode != *current_camera_mode) {
        // to set new mode: set mode and add wait for 3 seconds
        // (it takes time for camera to change mode sometimes)
         mavlink::Pld_mission_item::Ptr mi_set_camera_mode = mavlink::Pld_mission_item::Create();
        (*mi_set_camera_mode)->target_system = px4_vehicle.real_system_id;
        (*mi_set_camera_mode)->target_component = px4_vehicle.camera_component_id;
        (*mi_set_camera_mode)->command = mavlink::MAV_CMD::MAV_CMD_SET_CAMERA_MODE;
        (*mi_set_camera_mode)->param1 = 0; // reserved
        (*mi_set_camera_mode)->param2 = new_camera_mode;
        Add_mission_item(mi_set_camera_mode);

        current_camera_mode = new_camera_mode;

        // wait 3 seconds
        auto explicit_wait = Wait_action::Create(3);
        Prepare_action(explicit_wait);
    }
}

void
Px4_vehicle::Task_upload::Prepare_camera_trigger_impl(bool multiply_photos, float interval_in_seconds)
{
    // if no camera is found - use DO_REPEAT_SERVO command
    if (px4_vehicle.camera_trigger_type == 1) {
        mavlink::Pld_mission_item::Ptr mi = mavlink::Pld_mission_item::Create();
        (*mi)->command = mavlink::MAV_CMD::MAV_CMD_DO_REPEAT_SERVO;
        (*mi)->param1 = px4_vehicle.camera_servo_idx;
        (*mi)->param2 = px4_vehicle.camera_servo_pwm;
        (*mi)->param3 = multiply_photos ? 0xffff : 1;
        (*mi)->param4 = multiply_photos ? interval_in_seconds : px4_vehicle.camera_servo_time;
        Add_mission_item(mi);
    } else {
        Prepare_camera_mode(mavlink::CAMERA_MODE::CAMERA_MODE_IMAGE);

        mavlink::Pld_mission_item::Ptr mi_start_capture = mavlink::Pld_mission_item::Create();
        (*mi_start_capture)->target_system = px4_vehicle.real_system_id;
        (*mi_start_capture)->target_component = px4_vehicle.camera_component_id;
        (*mi_start_capture)->command = mavlink::MAV_CMD::MAV_CMD_IMAGE_START_CAPTURE;
        (*mi_start_capture)->param1 = 0;  // camera id. 0 means all cameras.
        (*mi_start_capture)->param2 = interval_in_seconds; // interval between photos.
        (*mi_start_capture)->param3 = multiply_photos ? 0 : 1; // zero means unlimited number of photos
        Add_mission_item(mi_start_capture);
    }
        camera_series_by_time_active = multiply_photos;
        camera_series_by_time_active_in_wp = multiply_photos;
}

void Px4_vehicle::Task_upload::Prepare_camera_recording_impl(bool start_recording) {
    Prepare_camera_mode(mavlink::CAMERA_MODE::CAMERA_MODE_VIDEO);

    mavlink::Pld_mission_item::Ptr mi_start_capture = mavlink::Pld_mission_item::Create();
    if (start_recording) {
        (*mi_start_capture)->command = mavlink::MAV_CMD::MAV_CMD_VIDEO_START_CAPTURE;
        (*mi_start_capture)->target_system = px4_vehicle.real_system_id;
        (*mi_start_capture)->target_component = px4_vehicle.camera_component_id;
        (*mi_start_capture)->param1 = 0; // Reserved (Set to 0)
        (*mi_start_capture)->param2 = 1; // Frequency CAMERA_CAPTURE_STATUS messages should be sent while
                                        // recording (0 for no messages, otherwise frequency in Hz)
    } else {
        (*mi_start_capture)->command = mavlink::MAV_CMD::MAV_CMD_VIDEO_STOP_CAPTURE;
        (*mi_start_capture)->target_system = px4_vehicle.real_system_id;
        (*mi_start_capture)->target_component = px4_vehicle.camera_component_id;
    }

    Add_mission_item(mi_start_capture);
}

void
Px4_vehicle::Task_upload::Prepare_camera_series_by_time(Action::Ptr& action)
{
    Camera_series_by_time_action::Ptr a =
            action->Get_action<Action::Type::CAMERA_SERIES_BY_TIME>();
    Prepare_camera_trigger_impl(true, static_cast<float>(a->interval.count()) / 1000.0);
}


void
Px4_vehicle::Task_upload::Prepare_camera_trigger(Action::Ptr& action)
{
    Camera_trigger_action::Ptr a =
        action->Get_action<Action::Type::CAMERA_TRIGGER>();
    switch (a->state) {
    case proto::CAMERA_MISSION_TRIGGER_STATE_SINGLE_PHOTO:
        Prepare_camera_trigger_impl(false);
        break;
    case proto::CAMERA_MISSION_TRIGGER_STATE_SERIAL_PHOTO:
        Prepare_camera_trigger_impl(true, static_cast<float>(a->interval.count()) / 1000.0);
        break;
    case proto::CAMERA_MISSION_TRIGGER_STATE_OFF:
    case proto::CAMERA_MISSION_TRIGGER_STATE_ON:
        if (px4_vehicle.camera_trigger_type == 0) {
            Prepare_camera_recording_impl(a->state == proto::CAMERA_MISSION_TRIGGER_STATE_ON);
        } else {
            VEHICLE_LOG_WRN(vehicle, "Unsupported camera trigger state %d ignored.", a->state);
        }
        break;
    }
}

void
Px4_vehicle::Task_upload::Prepare_camera_control(Action::Ptr& action)
{
    Camera_control_action::Ptr cam_control =
            action->Get_action<Action::Type::CAMERA_CONTROL>();

    mavlink::Pld_mission_item::Ptr mi = mavlink::Pld_mission_item::Create();
    (*mi)->command = mavlink::MAV_CMD::MAV_CMD_DO_MOUNT_CONTROL;

    /** In action target camera tilt value is in radians: [-Pi/2, Pi/2], where -Pi/2 stands
     * for looking backward, Pi/2 for full down and 0 for looking straight forward.
     *
     * In px4 gimbal 0 means looking forward and -90 degrees means looking down
     */
    (*mi)->param1 = -(cam_control->tilt) * 180.0 / M_PI;
    (*mi)->param2 = cam_control->roll * 180.0 / M_PI;
    (*mi)->param3 = cam_control->yaw * 180.0 / M_PI;
    (*mi)->z = mavlink::MAV_MOUNT_MODE::MAV_MOUNT_MODE_MAVLINK_TARGETING; // z means PARAM7
    Add_mission_item(mi);
}

mavlink::Pld_mission_item::Ptr
Px4_vehicle::Task_upload::Build_roi_mission_item(const Geodetic_tuple& coords)
{
    mavlink::Pld_mission_item::Ptr mi = mavlink::Pld_mission_item::Create();
    (*mi)->command = mavlink::MAV_CMD::MAV_CMD_DO_SET_ROI_LOCATION;
    Fill_mavlink_mission_item_coords(*mi, coords, 0);
    return mi;
}

mavlink::Pld_mission_item::Ptr
Px4_vehicle::Task_upload::Build_wp_mission_item(Action::Ptr& action)
{
    Move_action::Ptr ma = action->Get_action<Action::Type::MOVE>();
    mavlink::Pld_mission_item::Ptr mi = mavlink::Pld_mission_item::Create();

    (*mi)->command = mavlink::MAV_CMD::MAV_CMD_NAV_WAYPOINT;
    (*mi)->current = 0;

    (*mi)->param1 = ma->wait_time * 10;
    /* Set acceptance radius to something reasonable. */
    if (ma->acceptance_radius < ACCEPTANCE_RADIUS_MIN) {
        (*mi)->param2 = ACCEPTANCE_RADIUS_MIN;
        VEHICLE_LOG_INF(vehicle, "Acceptance radius normalized from %f to %f",
                ma->acceptance_radius, (*mi)->param2.Get());
    } else {
        (*mi)->param2 = ma->acceptance_radius;
    }
    (*mi)->param3 = ma->loiter_orbit;
    Fill_mavlink_mission_item_coords(*mi, ma->position.Get_geodetic(), ma->heading);
    return mi;
}

void
Px4_vehicle::Process_heartbeat(
            mavlink::Message<mavlink::MESSAGE_ID::HEARTBEAT>::Ptr message)
{
    // Process heartbeats only from vehicle
    if (!Is_vehicle_heartbeat_valid(message)) {
        return;
    }

    auto base_mode = Get_base_mode();
    if (base_mode & mavlink::MAV_MODE_FLAG::MAV_MODE_FLAG_CUSTOM_MODE_ENABLED) {
        auto new_mode = message->payload->custom_mode.Get();
        if (native_flight_mode.data != new_mode) {
            native_flight_mode.data = new_mode;
            const char* mode_name = Get_native_flight_mode_name(native_flight_mode);
            const auto main_mode = static_cast<Px4_main_mode>(native_flight_mode.main_mode);
            const auto sub_mode = static_cast<Px4_auto_sub_mode>(native_flight_mode.sub_mode);
            if (main_mode == Px4_main_mode::AUTO) {
                t_control_mode->Set_value(proto::CONTROL_MODE_AUTO);
                switch (sub_mode) {
                case Px4_auto_sub_mode::AUTO_LOITER:
                    t_control_mode->Set_value(proto::CONTROL_MODE_CLICK_GO);
                    current_flight_mode = proto::FLIGHT_MODE_HOLD;
                    break;
                case Px4_auto_sub_mode::AUTO_LAND:
                    current_flight_mode = proto::FLIGHT_MODE_LAND;
                    break;
                case Px4_auto_sub_mode::AUTO_MISSION:
                    current_flight_mode = proto::FLIGHT_MODE_WAYPOINTS;
                    break;
                case Px4_auto_sub_mode::AUTO_RTL:
                    current_flight_mode = proto::FLIGHT_MODE_RTH;
                    break;
                case Px4_auto_sub_mode::AUTO_TAKEOFF:
                    current_flight_mode = proto::FLIGHT_MODE_TAKEOFF;
                    break;
                default:
                    current_flight_mode.Disengage();
                    break;
                }
            } else {
                current_flight_mode.Disengage();
                t_control_mode->Set_value(proto::CONTROL_MODE_MANUAL);
            }
            VEHICLE_LOG_INF((*this),
                "Native flight mode changed to %s (%04X)",
                mode_name,
                new_mode);
            t_native_flight_mode->Set_value(mode_name);
        }
    } else if (base_mode & mavlink::MAV_MODE_FLAG::MAV_MODE_FLAG_AUTO_ENABLED) {
        // Handle case when px4 is disarmed without RC connected.
        if (Is_armed()) {
            t_control_mode->Set_value(proto::CONTROL_MODE_AUTO);
        } else {
            t_control_mode->Set_value(proto::CONTROL_MODE_MANUAL);
        }
    } else if (base_mode & mavlink::MAV_MODE_FLAG::MAV_MODE_FLAG_MANUAL_INPUT_ENABLED) {
        t_control_mode->Set_value(proto::CONTROL_MODE_AUTO);
    } else if (base_mode & mavlink::MAV_MODE_FLAG::MAV_MODE_FLAG_GUIDED_ENABLED) {
        t_control_mode->Set_value(proto::CONTROL_MODE_CLICK_GO);
    } else {
        t_control_mode->Set_value_na();
    }

    if (Is_control_mode(proto::CONTROL_MODE_MANUAL) && direct_vehicle_control) {
        t_control_mode->Set_value(proto::CONTROL_MODE_JOYSTICK);
    }

    bool was_armed = false;
    t_is_armed->Get_value(was_armed);

    if (Is_armed()) {
        t_is_armed->Set_value(true);
        if (!was_armed) {
            VEHICLE_LOG_INF(*this, "Vehicle ARMED");
        }
    } else {
        t_is_armed->Set_value(false);
        if (was_armed) {
            VEHICLE_LOG_INF(*this, "Vehicle DISARMED");
        }
    }

    if (current_flight_mode) {
        t_flight_mode->Set_value(*current_flight_mode);
    } else {
        t_flight_mode->Set_value_na();
    }

    Update_capability_states();
}

void
Px4_vehicle::Update_capability_states()
{
    int current_control_mode;
    t_control_mode->Get_value(current_control_mode);
    c_direct_vehicle_control->Set_enabled(Is_control_mode(proto::CONTROL_MODE_JOYSTICK));
    c_direct_vehicle_control->Set_available(Is_control_mode(proto::CONTROL_MODE_JOYSTICK));
    c_manual->Set_enabled(current_control_mode != proto::CONTROL_MODE_MANUAL);
    c_disarm->Set_enabled(Is_armed() && !is_airborne);
    if (Is_armed() && is_airborne) {
        c_waypoint->Set_enabled();
        c_emergency_land->Set_enabled();
        c_auto->Set_enabled(!Is_flight_mode(proto::FLIGHT_MODE_WAYPOINTS));
        c_guided->Set_enabled(current_control_mode != proto::CONTROL_MODE_CLICK_GO);
        c_land_command->Set_enabled();
        c_pause->Set_enabled(!Is_control_mode(proto::CONTROL_MODE_MANUAL) && !Is_flight_mode(proto::FLIGHT_MODE_HOLD));
        c_resume->Set_enabled(Is_flight_mode(proto::FLIGHT_MODE_HOLD));
        c_joystick->Set_enabled(!Is_control_mode(proto::CONTROL_MODE_JOYSTICK));
        c_rth->Set_enabled();
        c_takeoff_command->Set_enabled(false);
        c_arm->Set_enabled(false);
    } else {
        c_waypoint->Set_enabled(Is_armed());
        c_emergency_land->Set_enabled(false);
        c_auto->Set_enabled(Is_armed());
        c_guided->Set_enabled(false);
        c_land_command->Set_enabled(false);
        c_pause->Set_enabled(false);
        c_resume->Set_enabled(false);
        c_joystick->Set_enabled(false);
        c_rth->Set_enabled(false);
        c_takeoff_command->Set_enabled(Is_armed());
        c_arm->Set_enabled(!Is_armed() && current_control_mode != proto::CONTROL_MODE_AUTO);
    }
    Commit_to_ucs();
}

void
Px4_vehicle::On_extended_sys_state(
    mavlink::Message<mavlink::MESSAGE_ID::EXTENDED_SYS_STATE>::Ptr message)
{
    is_airborne = (message->payload->landed_state == mavlink::MAV_LANDED_STATE_IN_AIR);
    Update_capability_states();
}

void
Px4_vehicle::Configure_common()
{
    Set_rc_loss_actions({
        proto::FAILSAFE_ACTION_RTH,
        proto::FAILSAFE_ACTION_CONTINUE,
        proto::FAILSAFE_ACTION_WAIT,
        proto::FAILSAFE_ACTION_LAND
        });

    Set_low_battery_actions({
        proto::FAILSAFE_ACTION_RTH,
        proto::FAILSAFE_ACTION_CONTINUE,
        proto::FAILSAFE_ACTION_LAND
        });

    auto props = Properties::Get_instance().get();
    camera_trigger_type = props->Get_int("vehicle.px4.camera_trigger_type");
    camera_servo_idx = props->Get_int("vehicle.px4.camera_servo_idx");
    camera_servo_pwm = props->Get_int("vehicle.px4.camera_servo_pwm");
    camera_servo_time = props->Get_float("vehicle.px4.camera_servo_time");

    if (props->Exists("vehicle.px4.autoheading")) {
        auto yes = props->Get("vehicle.px4.autoheading");
        if (yes == "no") {
            autoheading = false;
        } else if (yes == "yes") {
            autoheading = true;
        } else {
            LOG_ERR("Invalid value '%s' for autoheading", yes.c_str());
        }

        if (autoheading) {
            VEHICLE_LOG_INF((*this), "Autoheading is on.");
        } else {
            VEHICLE_LOG_INF((*this), "Autoheading is off.");
        }
    }
}

void
Px4_vehicle::Configure_real_vehicle()
{
    auto props = Properties::Get_instance().get();
    if (props->Exists("vehicle.px4.enable_joystick_control_for_fixed_wing")) {
        auto yes = props->Get("vehicle.px4.enable_joystick_control_for_fixed_wing");
        if (yes == "yes") {
            LOG_INFO("Enabled joystick mode for fixed wing.");
            enable_joystick_control_for_fixed_wing = true;
        }
    }

    if (props->Exists("vehicle.px4.report_relative_altitude")) {
        auto yes = props->Get("vehicle.px4.report_relative_altitude");
        if (yes == "no") {
            report_relative_altitude = false;
            LOG_INFO("VSM will not report relative altitude.");
        } else if (yes == "yes") {
            report_relative_altitude = true;
            LOG_INFO("VSM will report relative altitude.");
        } else {
            LOG_ERR("Invalid value '%s' for report_relative_altitude", yes.c_str());
        }
    }

    // This is deprecated. use mavlink.protocol_version instead.
    if (props->Exists("vehicle.px4.mavlink_protocol_version")) {
        auto value = props->Get("vehicle.px4.mavlink_protocol_version");
        Trim(value);
        if (value == "1") {
            use_mavlink_2 = false;
            LOG_INFO("Force mavlink v1");
        } else if (value == "2") {
            use_mavlink_2 = true;
            LOG_INFO("Force mavlink v2");
        } else if (value == "auto") {
            use_mavlink_2.Disengage();
        } else {
            LOG_ERR("Invalid value '%s' for mavlink_protocol_version", value.c_str());
        }
    }

    telemetry_rates[mavlink::ALTITUDE] = DEFAULT_TELEMETRY_RATE;
    telemetry_rates[mavlink::ATTITUDE] = DEFAULT_TELEMETRY_RATE;
    telemetry_rates[mavlink::GLOBAL_POSITION_INT] = DEFAULT_TELEMETRY_RATE;
    telemetry_rates[mavlink::POSITION_TARGET_GLOBAL_INT] = DEFAULT_TELEMETRY_RATE;
    telemetry_rates[mavlink::GPS_RAW_INT] = DEFAULT_TELEMETRY_RATE;
    telemetry_rates[mavlink::HOME_POSITION] = DEFAULT_TELEMETRY_RATE;
    telemetry_rates[mavlink::HEARTBEAT] = DEFAULT_TELEMETRY_RATE;
    telemetry_rates[mavlink::SYS_STATUS] = DEFAULT_TELEMETRY_RATE;
    telemetry_rates[mavlink::VFR_HUD] = DEFAULT_TELEMETRY_RATE;

    for (auto it = props->begin("vehicle.px4.telemetry_rate", '.'); it != props->end(); it++)
    {
        float value = props->Get_float(*it);
        if (value < 0.1) {
            value = 0.1;
        } else if (value > 50) {
            value = 50;
        }
        if (it[3] == "ALTITUDE") {
            telemetry_rates[mavlink::ALTITUDE] = value;
        } else if (it[3] == "ATTITUDE") {
            telemetry_rates[mavlink::ATTITUDE] = value;
        } else if (it[3] == "GLOBAL_POSITION_INT") {
            telemetry_rates[mavlink::GLOBAL_POSITION_INT] = value;
        } else if (it[3] == "POSITION_TARGET_GLOBAL_INT") {
            telemetry_rates[mavlink::POSITION_TARGET_GLOBAL_INT] = value;
        } else if (it[3] == "GPS_RAW_INT") {
            telemetry_rates[mavlink::GPS_RAW_INT] = value;
        } else if (it[3] == "HOME_POSITION") {
            telemetry_rates[mavlink::HOME_POSITION] = value;
        } else if (it[3] == "HEARTBEAT") {
            telemetry_rates[mavlink::HEARTBEAT] = value;
        } else if (it[3] == "SYS_STATUS") {
            telemetry_rates[mavlink::SYS_STATUS] = value;
        } else if (it[3] == "VFR_HUD") {
            telemetry_rates[mavlink::VFR_HUD] = value;
        } else {
            LOG("Unsupported message type %s for telemetry_rate", it[3].c_str());
            continue;
        }
        LOG("Setting telemetry_rate for %s to %0.2f Hz", it[3].c_str(), value);
    }

    // We are counting 6 messages as telemetry:
    // SYS_STATUS, GLOBAL_POSITION_INT, ATTITUDE, VFR_HUD, GPS_RAW_INT, ALTITUDE
    expected_telemetry_rate =
        telemetry_rates[mavlink::ALTITUDE] +
        telemetry_rates[mavlink::ATTITUDE] +
        telemetry_rates[mavlink::GLOBAL_POSITION_INT] +
        telemetry_rates[mavlink::GPS_RAW_INT] +
        telemetry_rates[mavlink::SYS_STATUS] +
        telemetry_rates[mavlink::VFR_HUD];

    LOG("Setting expected telemetry_rate to %0.2f", expected_telemetry_rate);
}

const char*
Px4_vehicle::Get_native_flight_mode_name(Px4_custom_mode mode)
{
    switch (static_cast<Px4_main_mode>(mode.main_mode)) {
    case Px4_main_mode::ACRO:
        return "ACRO";
    case Px4_main_mode::ALTCTL:
        return "ALTCTL";
    case Px4_main_mode::AUTO:
        switch (static_cast<Px4_auto_sub_mode>(mode.sub_mode)) {
        case Px4_auto_sub_mode::AUTO_FOLLOW_TARGET:
            return "AUTO_FOLLOW_TARGET";
        case Px4_auto_sub_mode::AUTO_LAND:
            return "AUTO_LAND";
        case Px4_auto_sub_mode::AUTO_LOITER:
            return "AUTO_LOITER";
        case Px4_auto_sub_mode::AUTO_MISSION:
            return "AUTO_MISSION";
        case Px4_auto_sub_mode::AUTO_READY:
            return "AUTO_READY";
        case Px4_auto_sub_mode::AUTO_RTGS:
            return "AUTO_RTGS";
        case Px4_auto_sub_mode::AUTO_RTL:
            return "AUTO_RTL";
        case Px4_auto_sub_mode::AUTO_TAKEOFF:
            return "AUTO_TAKEOFF";
        case Px4_auto_sub_mode::UNKNOWN:
            return "AUTO_UNKNOWN";
        default:
            return "AUTO";
        }
        break;
    case Px4_main_mode::MANUAL:
        return "MANUAL";
    case Px4_main_mode::OFFBOARD:
        return "OFFBOARD";
    case Px4_main_mode::POSCTL:
        return "POSCTL";
    case Px4_main_mode::RATTITUDE:
        return "RATTITUDE";
    case Px4_main_mode::STABILIZED:
        return "STABILIZED";
    case Px4_main_mode::UNKNOWN:
        return "UNKNOWN";
    }
    return nullptr;
}
