/*******************************************************************************
  Copyright(c) 2024 BigPowerBox Contributors. All rights reserved.

  BigPowerBox INDI Driver - Simple Version

  This program is free software; you can redistribute it and/or modify it
  under the terms of the GNU General Public License as published by the Free
  Software Foundation; either version 2 of the License, or (at your option)
  any later version.

  This program is distributed in the hope that it will be useful, but WITHOUT
  ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
  FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
  more details.

  You should have received a copy of the GNU Library General Public License
  along with this library; see the file COPYING.LIB.  If not, write to
  the Free Software Foundation, Inc., 51 Franklin Street, Fifth Floor,
  Boston, MA 02110-1301, USA.

  The full GNU General Public License is included in this distribution in the
  file called LICENSE.
*******************************************************************************/

#include "bigpowerbox_simple.h"

#include "indicom.h"
#include "connectionplugins/connectionserial.h"

#include <cstring>
#include <sstream>
#include <memory>
#include <termios.h>
#include <unistd.h>

// Auto pointer to BigPowerBoxSimple
static std::unique_ptr<BigPowerBoxSimple> bigpowerbox_simple(new BigPowerBoxSimple());

#define TIMEOUT_MSEC 1000

BigPowerBoxSimple::BigPowerBoxSimple()
{
    setVersion(1, 0);
}

const char *BigPowerBoxSimple::getDefaultName()
{
    return "BigPowerBox Simple";
}

bool BigPowerBoxSimple::initProperties()
{
    INDI::DefaultDevice::initProperties();
    
    // Add standard controls
    addAuxControls();
    addDebugControl();

    // Set device interface
    setDriverInterface(AUX_INTERFACE);

    // // Serial Connection
    serialConnection = new Connection::Serial(this);
    serialConnection->registerHandshake([&]()
    {
        return Handshake();
    });
    serialConnection->setDefaultBaudRate(Connection::Serial::B_9600);
    registerConnection(serialConnection);

    m_Initialized = true;
    LOG_INFO("initProperties complete");
    
    return true;
}

bool BigPowerBoxSimple::updateProperties()
{
    if (!m_Initialized)
    {
        LOG_INFO("updateProperties called before initialization complete, skipping");
        return true;
    }
    
    LOGF_DEBUG("updateProperties called, isConnected=%d", isConnected());
    
    INDI::DefaultDevice::updateProperties();

    if (isConnected())
    {
        LOG_INFO("updateProperties: Device is connected");
    }
    else
    {
        LOG_INFO("updateProperties: Device is disconnected");
    }

    return true;
}

bool BigPowerBoxSimple::ISNewSwitch(const char *dev, const char *name, ISState *states, char *names[], int n)
{
    if (dev && !strcmp(dev, getDeviceName()))
    {
        LOG_INFO("ISNewSwitch called for our device");
        // Handle custom switches here in the future
    }

    // Let parent class handle standard switches (DEBUG, CONFIG, CONNECTION, etc.)
    return INDI::DefaultDevice::ISNewSwitch(dev, name, states, names, n);
}

bool BigPowerBoxSimple::Handshake()
{
    // Small delay to stabilize connection before returning success
    usleep(100000); // 100ms
    LOG_INFO("Handshake complete");
    return true;
}

// bool BigPowerBoxSimple::Handshake()
// {
//     return true;
//     LOG_DEBUG("Starting handshake...");
    
//     PortFD = serialConnection->getPortFD();
    
//     if (PortFD <= 0)
//     {
//         LOGF_ERROR("Invalid port file descriptor during handshake (PortFD=%d).", PortFD);
//         return false;
//     }
    
//     LOGF_DEBUG("Handshake: PortFD=%d", PortFD);
    
//     // Wait for connection to stabilize
//     LOG_DEBUG("Waiting for connection to stabilize...");
//     usleep(500000); // 500ms delay
    
//     // Flush any stale data
//     LOG_DEBUG("Flushing serial buffers...");
//     tcflush(PortFD, TCIOFLUSH);
    
//     // Ping device (retry up to 3 times)
//     LOG_DEBUG("Starting ping attempts...");
//     for (int attempt = 0; attempt < 3; attempt++)
//     {
//         if (ping())
//         {
//             LOG_DEBUG("Ping successful!");
//             break;
//         }
//         if (attempt < 2)
//         {
//             LOGF_WARN("Ping attempt %d failed, retrying...", attempt + 1);
//             usleep(500000); // 500ms delay
//         }
//         else
//         {
//             LOG_ERROR("Failed to ping device after 3 attempts");
//             return false;
//         }
//     }

//     // Discover device
//     LOG_DEBUG("Sending discover command...");
//     if (!discover())
//     {
//         LOG_ERROR("Discovery failed");
//         return false;
//     }

//     LOGF_INFO("Device: %s, Version: %s, Signature: %s", 
//               m_DeviceName.c_str(), m_Version.c_str(), m_BoardSignature.c_str());

//     return true;
// }

// bool BigPowerBoxSimple::sendCommand(const char *command, std::string &response, int timeoutMs)
// {
//     if (PortFD < 0)
//     {
//         LOGF_ERROR("Invalid PortFD (%d) in sendCommand", PortFD);
//         return false;
//     }

//     // Flush both input and output buffers to ensure clean state
//     tcflush(PortFD, TCIOFLUSH);

//     // Send command with newline
//     char commandWithNewline[256];
//     snprintf(commandWithNewline, sizeof(commandWithNewline), "%s\n", command);
//     LOGF_DEBUG("SEND: %s", command);
    
//     int nbytes_written = 0;
//     if (tty_write(PortFD, commandWithNewline, strlen(commandWithNewline), &nbytes_written) != TTY_OK)
//     {
//         char errmsg[256];
//         tty_error_msg(nbytes_written, errmsg, 256);
//         LOGF_ERROR("Serial write error: %s", errmsg);
//         return false;
//     }
    
//     LOGF_DEBUG("Wrote %d bytes", nbytes_written);
    
//     // Wait for data to be transmitted
//     tcdrain(PortFD);
    
//     // Small delay to allow device to process command
//     usleep(100000); // 100ms delay

//     // Read response until '#' terminator
//     char buffer[512];
//     int nbytes_read = 0;
//     int timeoutSeconds = (timeoutMs + 999) / 1000; // Convert ms to seconds, rounding up
//     LOGF_DEBUG("Reading response (timeout: %d seconds)...", timeoutSeconds);
    
//     int tty_result = tty_read_section(PortFD, buffer, '#', timeoutSeconds, &nbytes_read);
//     if (tty_result != TTY_OK)
//     {
//         if (tty_result == TTY_TIME_OUT)
//         {
//             LOGF_ERROR("Read timeout after %d seconds", timeoutSeconds);
//         }
//         else
//         {
//             char errmsg[256];
//             tty_error_msg(tty_result, errmsg, 256);
//             LOGF_ERROR("Serial read error: %s (code=%d)", errmsg, tty_result);
//         }
//         return false;
//     }

//     buffer[nbytes_read] = '\0';
//     response = buffer;
    
//     LOGF_DEBUG("RECV (%d bytes): %s", nbytes_read, response.c_str());
//     return true;
// }

// bool BigPowerBoxSimple::ping()
// {
//     LOG_DEBUG("Attempting ping...");
//     std::string response;
//     if (!sendCommand(">P#", response, TIMEOUT_MSEC))
//     {
//         LOG_DEBUG("Ping command failed or timed out");
//         return false;
//     }
    
//     LOGF_DEBUG("Ping response received: %s", response.c_str());
    
//     // Response should be ">POK#" but may have trailing characters
//     bool success = response.find(">POK") != std::string::npos;
//     if (!success)
//     {
//         LOGF_ERROR("Ping failed: Expected '>POK' in response, got: '%s'", response.c_str());
//     }
//     return success;
// }

// bool BigPowerBoxSimple::discover()
// {
//     std::string response;
//     if (!sendCommand(">D#", response, TIMEOUT_MSEC))
//     {
//         LOG_ERROR("Discover command failed or timed out");
//         return false;
//     }
    
//     LOGF_DEBUG("Discover response: %s", response.c_str());
    
//     std::vector<std::string> fields;
//     splitFields(response, fields);
    
//     // Expected format: >D:name:version:signature#
//     if (fields.size() < 4 || fields[0] != "D")
//     {
//         LOGF_ERROR("Invalid discover response format. Expected >D:name:version:signature#, got: %s", response.c_str());
//         return false;
//     }
    
//     m_DeviceName = fields[1];
//     m_Version = fields[2];
//     m_BoardSignature = fields[3];
    
//     LOGF_INFO("Discovered device: %s (version %s, signature: %s)", 
//               m_DeviceName.c_str(), m_Version.c_str(), m_BoardSignature.c_str());
    
//     return true;
// }

// void BigPowerBoxSimple::splitFields(const std::string &response, std::vector<std::string> &fields)
// {
//     fields.clear();
    
//     // Remove '>' and '#' markers
//     std::string data = response;
//     if (!data.empty() && data[0] == '>')
//         data = data.substr(1);
//     if (!data.empty() && data.back() == '#')
//         data = data.substr(0, data.length() - 1);
    
//     // Split by ':'
//     std::istringstream iss(data);
//     std::string field;
    
//     while (std::getline(iss, field, ':'))
//     {
//         fields.push_back(field);
//     }
// }
