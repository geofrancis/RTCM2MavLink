
#include <WiFi.h>  //Need for ESP32
#include "NTRIPClient.h"
#include <HardwareSerial.h>
#include "mavlink/common/mavlink.h"
#include "functions.h"
//Define two Serial devices mapped to the two internal UARTs
HardwareSerial MySerial0(0);
HardwareSerial MAVLINK_UART(1);

const char* ssid = "2.4GN";
const char* password = "passw0rded";

char* host = "rtk2go.com";
int httpPort = 2101;  //port 2101 is default port of NTRIP caster
char* mntpnt = "Kilmorna";
char* user = "user-at-gemail.com";
char* passwd = "none";
NTRIPClient ntrip_c;
int ledState = LOW;


#include <NTRIPClient.h>
#define RTCM_BUFFER_SIZE 720
#define RTCM_MAX_FRAGMENTS 4  // Maximum number of fragments for an RTCM message

#define SYSTEM_ID 255     // System ID For MAVLINK
#define COMPONENT_ID 41  // Component ID for MAVLINK

// RTCM Variables.
byte incomingBytes[RTCM_BUFFER_SIZE];  // Buffer for incoming bytes
byte rtcmBuffer[RTCM_BUFFER_SIZE];     // Buffer for the captured message
int incomingIndex = 0;                 // Current index in incomingBytes
int rtcmIndex = 0;                     // Current index in rtcmBuffer
u_int32_t lastRtcmMillis;              // How long since we received an rtcm message.

// MAVLINK Variables.
uint8_t _sequenceId;
//uint32_t heartbeatDetectedMillis; // how long has it been since we saw our first heartbeat (and play a tone or something).
uint32_t heartbeatMillis;






void setup() {

  pinMode(8, OUTPUT);

  Serial.begin(115200);
  MAVLINK_UART.begin(115200, SERIAL_8N1, 9, 10);


  delay(10);
  Serial.print("Connecting to ");
  Serial.println(ssid);
  WiFi.begin(ssid, password);
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }
  Serial.println("WiFi connected");
  Serial.println("IP address: ");
  Serial.println(WiFi.localIP());

  Serial.println("Requesting SourceTable.");
  if (ntrip_c.reqSrcTbl(host, httpPort)) {
    char buffer[512];
    delay(5);
    while (ntrip_c.available()) {
      ntrip_c.readLine(buffer, sizeof(buffer));
      Serial.print(buffer);
    }
  } else {
    Serial.println("SourceTable request error");
  }
  Serial.print("Requesting SourceTable is OK\n");
  ntrip_c.stop();  //Need to call "stop" function for next request.

  Serial.println("Requesting MountPoint's Raw data");
  if (!ntrip_c.reqRaw(host, httpPort, mntpnt, user, passwd)) {
    delay(15000);
    ESP.restart();
  }
  Serial.println("Requesting MountPoint is OK");
}

void loop() {

  SendMavlinkHeartbeat();
  ReadRTCM();
  ReadMAVLINK();
}






void ReadRTCM() {
  // Read data from RTCM_UART
  while (ntrip_c.available() > 0) {
    byte byteRead = ntrip_c.read();  // Read a byte from RTCM_UART

    // Store the byte in the incomingBytes buffer if there's space
    if (incomingIndex < RTCM_BUFFER_SIZE) {
      incomingBytes[incomingIndex++] = byteRead;  // Add byte to incomingBytes
    } else {
      incomingIndex = 0;  // Reset index if overflow occurs
    }

    // Check for the delimiter in the incomingBytes buffer
    if (incomingIndex >= 2) {
      for (int i = 0; i <= incomingIndex - 2; i++) {
        // Check for the delimiter
        if (incomingBytes[i] == 0xD3 && (incomingBytes[i + 1] & 0xF8) == 0x00) {

          // Reset the rtcmBuffer index
          rtcmIndex = 0;
          memset(rtcmBuffer, 0, sizeof(rtcmBuffer));

          // Store the delimiter and all previous bytes in rtcmBuffer
          if (rtcmIndex + incomingIndex <= RTCM_BUFFER_SIZE) {

            // Store delimiter
            rtcmBuffer[rtcmIndex++] = incomingBytes[i];
            rtcmBuffer[rtcmIndex++] = incomingBytes[i + 1];

            // Store all bytes before the delimiter
            for (int j = 0; j < i; j++) {
              rtcmBuffer[rtcmIndex++] = incomingBytes[j];
            }
          }

          // Reset the incomingIndex to start over after the delimiter
          incomingIndex = 0;

          if (ledState == LOW) {
            ledState = HIGH;
          } else {
            ledState = LOW;
          }

          // set the LED with the ledState of the variable:
          digitalWrite(8, ledState);

          //Optional: Print the stored message for debugging
          //Serial.print("RTCM: ");
          //Serial.print(rtcmIndex);
          //Serial.print(" ");
         // for (int k = 0; k < rtcmIndex; k++) {
         //   Serial.print(rtcmBuffer[k], HEX);
          //  Serial.print(" ");
         // }
        //  Serial.println();

          DumpRTCM3(rtcmIndex, rtcmBuffer);
          lastRtcmMillis = millis();  // update time since.  This really should only be updated if verified message in above function.

          // SEND THE MAVLINK MESSAGE NOW.  This should be a function of course.
          mavlink_message_t mavmsg;
          uint8_t buf[MAVLINK_MAX_PACKET_LEN];

          // Determine if we need to fragment the packet into smaller chunks.
          if (rtcmIndex < 179) {
            // Fill the MAVLink message with the RTCM data
            mavlink_msg_gps_rtcm_data_pack(
              SYSTEM_ID,     // system id
              COMPONENT_ID,  // component id
              &mavmsg,
              (_sequenceId & 0x1F) << 3,
              rtcmIndex,  // data length
              rtcmBuffer  // data
            );

            uint16_t len = mavlink_msg_to_send_buffer(buf, &mavmsg);
            MAVLINK_UART.write(buf, len);
          } else {

            // We need to fragment - ToDO: SHOULD WARN IF > RTCM_MAX_FRAGMENTS which probably never happens but just in case.
            Serial.println("RTCM needs to be fragmented.");
            uint8_t fragmentId = 0;  // Fragment id indicates the fragment within a set
            int start = 0;

            while (start < rtcmIndex) {
              int l = std::min(rtcmIndex - start, 179);
              uint8_t msgpart[l];

              uint8_t flags = 1;                   // LSB set indicates message is fragmented
              flags |= fragmentId++ << 1;          // Next 2 bits are fragment id (SHOULD WARN IF > RTCM_MAX_FRAGMENTS)
              flags |= (_sequenceId & 0x1F) << 3;  // Next 5 bits are sequence id

              Serial.printf("Sequence ID: %d Fragment ID: %d Start: %d Len: %d\n", _sequenceId, fragmentId, start, l);

              memcpy(&rtcmBuffer, msgpart + start, l);

              // Fill the MAVLink message with the RTCM data
              mavlink_msg_gps_rtcm_data_pack(
                SYSTEM_ID,     // system id
                COMPONENT_ID,  // component id
                &mavmsg,
                flags,
                l,       // data length
                msgpart  // data
              );

              uint16_t len = mavlink_msg_to_send_buffer(buf, &mavmsg);
              MAVLINK_UART.write(buf, len);

              start += l;
            }
            //    }
          }

          ++_sequenceId;

          break;  // Exit the loop after handling the delimiter
        }
      }
    }
  }
}

void SendMavlinkHeartbeat() {

  if (millis() - heartbeatMillis > 1000 ) {

    // Send HEARTBEAT message to serial once a second
    mavlink_message_t msg;
    uint8_t buf[MAVLINK_MAX_PACKET_LEN];

    // uint16_t mavlink_msg_heartbeat_pack(uint8_t system_id, uint8_t component_id, mavlink_message_t* msg,
    //                                uint8_t type, uint8_t autopilot, uint8_t base_mode, uint32_t custom_mode, uint8_t system_status)

    mavlink_msg_heartbeat_pack(SYSTEM_ID, COMPONENT_ID, &msg, MAV_TYPE_ONBOARD_CONTROLLER, MAV_AUTOPILOT_INVALID, MAV_MODE_FLAG_MANUAL_INPUT_ENABLED, 0, MAV_STATE_STANDBY);
    uint16_t len = mavlink_msg_to_send_buffer(buf, &msg);

    MAVLINK_UART.write(buf, len);

    heartbeatMillis = millis();
  }
}

void RequestParam()  // for future use...
{

  uint8_t target_system = 1;
  uint8_t target_component = 0;

  // Send REQUEST message to Serial
  mavlink_message_t msg;
  uint8_t buf[MAVLINK_MAX_PACKET_LEN];

  // Pack
  //  uint16_t mavlink_msg_param_request_read_pack(uint8_t system_id, uint8_t component_id, mavlink_message_t* msg,
  //                               uint8_t target_system, uint8_t target_component, const char *param_id, int16_t param_index)
  //
  mavlink_msg_param_request_read_pack(SYSTEM_ID, COMPONENT_ID, &msg, target_system, target_component, "GPS_DRV_OPTIONS", -1);
  uint16_t len = mavlink_msg_to_send_buffer(buf, &msg);

  // Send
  MAVLINK_UART.write(buf, len);
}



void ReadMAVLINK()  // probably not going to receive anything on a one way transmit only - but for testing locally we do.
{
  // pieces taken from various online sources.

  mavlink_message_t msg;
  mavlink_status_t status;

  while (MAVLINK_UART.available() > 0) {
    uint8_t c = MAVLINK_UART.read();

    // Try to get a new message
    if (mavlink_parse_char(MAVLINK_COMM_0, c, &msg, &status)) {

      Serial.printf("MavMsg->(%d), Seq: %d CMPID:%d SYSID:%d\n", msg.msgid, msg.seq, msg.compid, msg.sysid);

      // Handle message
      switch (msg.msgid) {
        case MAVLINK_MSG_ID_HEARTBEAT:  // #0: Heartbeat
          {
            // E.g. read GCS heartbeat and go into
            // comm lost mode if timer times out
            // Serial.println("Heartbeat.");
          }
          break;

        case MAVLINK_MSG_ID_SYS_STATUS:  // #1: SYS_STATUS
          {
            /* Message decoding: PRIMITIVE
              mavlink_msg_sys_status_decode(const mavlink_message_t* msg, mavlink_sys_status_t* sys_status)
        */
            mavlink_message_t* msg;
            mavlink_sys_status_t sys_status;

            // mavlink_msg_sys_status_decode(&msg, &sys_status);
            //            Serial.print("PX SYS STATUS: ");
            //            Serial.print("[Bat (V): ");
            //            Serial.print(sys_status.voltage_battery);
            //            Serial.print("], [Bat (A): ");
            //            Serial.print(sys_status.current_battery);
            //            Serial.print("], [Comms loss (%): ");
            //            Serial.print(sys_status.drop_rate_comm);
            //            Serial.println("]");
          }
          break;

        case MAVLINK_MSG_ID_PARAM_VALUE:  // #22: PARAM_VALUE
          {
            /* Message decoding: PRIMITIVE
              mavlink_msg_param_value_decode(const mavlink_message_t* msg, mavlink_param_value_t* param_value)
        */
            // mavlink_message_t* msg;
            mavlink_param_value_t param_value;
            mavlink_msg_param_value_decode(&msg, &param_value);

            Serial.printf("PARAM Received: %s\n", &param_value.param_id);
          }
          break;

        case MAVLINK_MSG_ID_RAW_IMU:  // #27: RAW_IMU
          {
            /* Message decoding: PRIMITIVE
              static inline void mavlink_msg_raw_imu_decode(const mavlink_message_t* msg, mavlink_raw_imu_t* raw_imu)
        */
            mavlink_raw_imu_t raw_imu;
            mavlink_msg_raw_imu_decode(&msg, &raw_imu);
          }
          break;

        case MAVLINK_MSG_ID_ATTITUDE:  // #30
          {
            /* Message decoding: PRIMITIVE
              mavlink_msg_attitude_decode(const mavlink_message_t* msg, mavlink_attitude_t* attitude)
        */
            mavlink_attitude_t attitude;
            mavlink_msg_attitude_decode(&msg, &attitude);

            //            if(attitude.roll>1) leds_modo = 0;
            //            else if(attitude.roll<-1) leds_modo = 2;
            //            else leds_modo=1;
          }
          break;

        default:
          break;
      }
    }
  }
}
