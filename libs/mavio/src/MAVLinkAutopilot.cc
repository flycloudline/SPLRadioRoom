/*
 MAVLinkAutopilot.cc
 
 (C) Copyright 2019 Envirover.

 This library is free software; you can redistribute it and/or
 modify it under the terms of the GNU Lesser General Public
 License as published by the Free Software Foundation; either
 version 2.1 of the License, or (at your option) any later version.

 This library is distributed in the hope that it will be useful,
 but WITHOUT ANY WARRANTY; without even the implied warranty of
 MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 Lesser General Public License for more details.

 You should have received a copy of the GNU Lesser General Public
 License along with this library; if not, write to the Free Software
 Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
*/

#include "MAVLinkAutopilot.h"

#include <chrono>

#include "MAVLinkLogger.h"

namespace mavio {

using std::string;
using std::vector;
using std::chrono::duration_cast;
using std::chrono::high_resolution_clock;
using std::chrono::milliseconds;

constexpr int send_retries = 5;
constexpr int receive_retries = 10;
constexpr int64_t max_heartbeat_interval = 2000;  // ms

constexpr struct timespec autopilot_send_interval[] = {{0, 10000000L}};  // 10ms
constexpr struct timespec receive_retry_delay[] = {{0, 10000000L}};      // 10ms

constexpr size_t max_autopilot_queue_size = 10;

MAVLinkAutopilot::MAVLinkAutopilot()
    : MAVLinkChannel("autopilot"),
      running(false),
      send_thread(),
      receive_thread(),
      serial(),
      send_queue(max_autopilot_queue_size),
      receive_queue(max_autopilot_queue_size),
      system_id(0) {}

bool MAVLinkAutopilot::init(const string& path, int speed,
                            const vector<string>& devices) {
  bool ret = connect(path, speed, devices);

  if (!running) {
    running = true;

    std::thread send_th(&MAVLinkAutopilot::send_task, this);
    send_thread.swap(send_th);

    std::thread receive_th(&MAVLinkAutopilot::receive_task, this);
    receive_thread.swap(receive_th);
  }

  return ret;
}

void MAVLinkAutopilot::close() {
  if (running) {
    running = false;

    receive_thread.join();
    send_thread.join();
  }

  serial.close();
}

bool MAVLinkAutopilot::request_autopilot_version(
    uint8_t& autopilot, uint8_t& mav_type, uint8_t& sys_id,
    mavlink_autopilot_version_t& autopilot_version) {
  mavlink_message_t msg, msg_command_long;
  autopilot = mav_type = sys_id = 0;
  memset(&autopilot_version, 0, sizeof(autopilot_version));

  for (high_resolution_clock::time_point start = high_resolution_clock::now();
       duration_cast<milliseconds>(high_resolution_clock::now() - start)
           .count() < max_heartbeat_interval;) {
    if (serial.receive_message(msg)) {
      if (msg.msgid == MAVLINK_MSG_ID_HEARTBEAT) {
        autopilot = mavlink_msg_heartbeat_get_autopilot(&msg);
        mav_type = mavlink_msg_heartbeat_get_type(&msg);
        sys_id = msg.sysid;

        if (autopilot != MAV_AUTOPILOT_INVALID)  // Filter out heartbeat
                                                 // messages forwarded from GCS
          break;
      }
    }

    nanosleep(receive_retry_delay, NULL);
  }

  // Return false if heartbeat message was not received
  if (sys_id == 0) {
    mavio::log(LOG_DEBUG, "Heartbeat not received.\n");
    return false;
  }

  for (int i = 0; i < send_retries; i++) {
    mavlink_msg_command_long_pack(system_id, component_id, &msg_command_long,
                                  sys_id, ardupilot_component_id,
                                  MAV_CMD_REQUEST_AUTOPILOT_CAPABILITIES, i,
                                  1.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0);
    if (serial.send_message(msg_command_long)) {
      for (int j = 0; j < receive_retries; j++) {
        if (serial.receive_message(msg)) {
          // printf("**** msg.msgid = %d\n", msg.msgid);
          if (msg.msgid == MAVLINK_MSG_ID_AUTOPILOT_VERSION) {
            mavlink_msg_autopilot_version_decode(&msg, &autopilot_version);
            sys_id = msg.sysid;
            return true;
          }
        }
      }
    } else {
      mavio::log(LOG_DEBUG, "Failed to send message to autopilot.\n");
    }

    nanosleep(receive_retry_delay, NULL);
  }

  return true;
}

char* MAVLinkAutopilot::get_firmware_version(
    const mavlink_autopilot_version_t& autopilot_version, char* buff,
    size_t buff_size) {
  strncpy(buff, "unknown", buff_size);

  if (autopilot_version.flight_sw_version != 0) {
    int majorVersion, minorVersion, patchVersion;
    FIRMWARE_VERSION_TYPE versionType;

    majorVersion = (autopilot_version.flight_sw_version >> (8 * 3)) & 0xFF;
    minorVersion = (autopilot_version.flight_sw_version >> (8 * 2)) & 0xFF;
    patchVersion = (autopilot_version.flight_sw_version >> (8 * 1)) & 0xFF;
    versionType = (FIRMWARE_VERSION_TYPE)(
        (autopilot_version.flight_sw_version >> (8 * 0)) & 0xFF);

    snprintf(buff, buff_size, "%d.%d.%d/%d ", majorVersion, minorVersion,
             patchVersion, versionType);
  }

  return buff;
}

bool MAVLinkAutopilot::send_message(const mavlink_message_t& msg) {
  send_queue.push(msg);
  return true;
}

bool MAVLinkAutopilot::receive_message(mavlink_message_t& msg) {
  return receive_queue.pop(msg);
}

bool MAVLinkAutopilot::message_available() { return !receive_queue.empty(); }

std::chrono::high_resolution_clock::time_point
MAVLinkAutopilot::last_send_time() {
  return send_queue.last_push_time();
}

std::chrono::high_resolution_clock::time_point
MAVLinkAutopilot::last_receive_time() {
  return receive_queue.last_push_time();
}

bool MAVLinkAutopilot::connect(const string& path, int speed,
                               const vector<string>& devices) {
  mavio::log(LOG_NOTICE, "Connecting to autopilot (%s %d)...", path.data(),
             speed);

  system_id = 0;

  if (serial.init(path, speed)) {
    system_id = detect_autopilot(path);

    if (system_id) {
      return true;
    }

    serial.close();
  } else {
    mavio::log(LOG_WARNING, "Failed to open serial device '%s'.", path.data());
  }

  if (devices.size() > 0) {
    mavio::log(LOG_NOTICE,
               "Attempting to detect autopilot at the available serial "
               "devices...");
    for (size_t i = 0; i < devices.size(); i++) {
      if (devices[i] == path) continue;

      if (serial.init(devices[i], speed) == 0) {
        system_id = detect_autopilot(devices[i].data());
        if (system_id) {
          return true;
        }

        serial.close();
      } else {
        mavio::log(LOG_DEBUG, "Failed to open serial device '%s'.",
                   devices[i].data());
      }
    }
  }

  serial.init(path, speed);

  mavio::log(LOG_ERR,
             "Autopilot was not detected on any of the serial devices.");

  return false;
}

uint8_t MAVLinkAutopilot::detect_autopilot(const string device) {
  mavlink_autopilot_version_t autopilot_version;
  uint8_t autopilot, mav_type, sys_id;

  if (!request_autopilot_version(autopilot, mav_type, sys_id,
                                 autopilot_version)) {
    mavio::log(LOG_DEBUG, "Autopilot not detected at serial device '%s'.",
               device.data());
    return 0;
  }

  char buff[64];
  get_firmware_version(autopilot_version, buff, sizeof(buff));

  mavio::log(LOG_NOTICE, "Autopilot detected at serial device '%s'.",
             device.data());
  mavio::log(
      LOG_NOTICE,
      "MAV type: %d, system id: %d, autopilot class: %d, firmware version: "
      "%s",
      mav_type, sys_id, autopilot, buff);

  return sys_id;
}

void MAVLinkAutopilot::send_task() {
  while (running) {
    mavlink_message_t msg;

    if (send_queue.pop(msg)) {
      serial.send_message(msg);
    }

    nanosleep(autopilot_send_interval, NULL);
  }
}

void MAVLinkAutopilot::receive_task() {
  while (running) {
    mavlink_message_t msg;

    if (serial.receive_message(msg)) {
      receive_queue.push(msg);
    }
  }
}

}  // namespace mavio
