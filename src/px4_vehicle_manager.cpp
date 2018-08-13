// Copyright (c) 2018, Smart Projects Holdings Ltd
// All rights reserved.
// See LICENSE file for license details.

#include <px4_vehicle_manager.h>
#include <px4_vehicle.h>
#include <ugcs/vsm/transport_detector.h>

using namespace ugcs::vsm;

Px4_vehicle_manager::Px4_vehicle_manager() :
Mavlink_vehicle_manager(
        "PX4",
        "vehicle.px4")
{
}

void
Px4_vehicle_manager::Register_detectors()
{
    Transport_detector::Get_instance()->Add_detector(
        ugcs::vsm::Transport_detector::Make_connect_handler(
            &Px4_vehicle_manager::Handle_new_connection,
            Shared_from_this(),
            ugcs::vsm::mavlink::MAV_AUTOPILOT_PX4,
            ugcs::vsm::Optional<std::string>(),
            ugcs::vsm::Optional<std::string>()),
        Shared_from_this());
}

Mavlink_vehicle::Ptr
Px4_vehicle_manager::Create_mavlink_vehicle(
        Mavlink_demuxer::System_id system_id,
        Mavlink_demuxer::Component_id component_id,
        mavlink::MAV_TYPE type,
        Io_stream::Ref stream,
        ugcs::vsm::Socket_address::Ptr,
        ugcs::vsm::Optional<std::string> mission_dump_path,
        const std::string& serial_number,
        const std::string& model_name)
{
    return Px4_vehicle::Create(
            system_id,
            component_id,
            type,
            stream,
            mission_dump_path,
            serial_number,
            model_name);
}
