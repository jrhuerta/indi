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
#include "inditimer.h"

#include <cstring>
#include <sstream>
#include <memory>
#include <termios.h>
#include <unistd.h>
#include <cmath>
#include <algorithm>

// Auto pointer to BigPowerBoxSimple
static std::unique_ptr<BigPowerBoxSimple> bigpowerbox_simple(new BigPowerBoxSimple());

#define TIMEOUT_MSEC 1000
#define UPDATE_INTERVAL_MS 2000

BigPowerBoxSimple::BigPowerBoxSimple() : INDI::PowerInterface(this)
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
    setDriverInterface(AUX_INTERFACE | POWER_INTERFACE);

    // Serial Connection
    serialConnection = new Connection::Serial(this);
    serialConnection->registerHandshake([&]()
    {
        return Handshake();
    });
    serialConnection->setDefaultBaudRate(Connection::Serial::B_9600);
    registerConnection(serialConnection);
    
    return true;
}

bool BigPowerBoxSimple::updateProperties()
{
    INDI::DefaultDevice::updateProperties();

    if (isConnected())
    {
        PI::updateProperties();
        setupComplete = true;
        // Start status polling timer
        SetTimer(UPDATE_INTERVAL_MS);
    }
    else
    {
        PI::updateProperties();
        setupComplete = false;
    }

    return true;
}

bool BigPowerBoxSimple::ISNewSwitch(const char *dev, const char *name, ISState *states, char *names[], int n)
{
    // Let parent class handle standard controls first
    if (INDI::DefaultDevice::ISNewSwitch(dev, name, states, names, n))
        return true;
    
    if (dev != nullptr && strcmp(dev, getDeviceName()) == 0)
    {
        // Delegate PowerInterface switches
        if (PI::processSwitch(dev, name, states, names, n))
            return true;
    }
    
    return false;
}

bool BigPowerBoxSimple::ISNewNumber(const char *dev, const char *name, double values[], char *names[], int n)
{
    if (dev != nullptr && strcmp(dev, getDeviceName()) == 0)
    {
        // Delegate PowerInterface numbers
        if (PI::processNumber(dev, name, values, names, n))
            return true;
    }
    
    return INDI::DefaultDevice::ISNewNumber(dev, name, values, names, n);
}

bool BigPowerBoxSimple::ISNewText(const char *dev, const char *name, char *texts[], char *names[], int n)
{
    if (dev != nullptr && strcmp(dev, getDeviceName()) == 0)
    {
        // Handle PowerInterface label properties
        // Power Channel Labels
        if (!strcmp(name, PI::PowerChannelLabelsTP.getName()))
        {
            PI::processText(dev, name, texts, names, n);
            
            // Update port names on device
            int changedCount = 0;
            for (size_t i = 0; i < PI::PowerChannelLabelsTP.size() && i < m_DCPortMap.size(); i++)
            {
                if (PI::PowerChannelLabelsTP[i].getText() != nullptr)
                {
                    int physicalPort = m_DCPortMap[i];
                    if (setPortName(physicalPort, PI::PowerChannelLabelsTP[i].getText()))
                    {
                        changedCount++;
                    }
                }
            }
            
            if (changedCount > 0)
            {
                LOGF_INFO("Updated %d DC port label(s)", changedCount);
            }
            
            PI::PowerChannelLabelsTP.setState(IPS_OK);
            PI::PowerChannelLabelsTP.apply();
            return true;
        }
        
        // Dew Channel Labels
        if (!strcmp(name, PI::DewChannelLabelsTP.getName()))
        {
            PI::processText(dev, name, texts, names, n);
            
            // Update port names on device
            int changedCount = 0;
            for (size_t i = 0; i < PI::DewChannelLabelsTP.size() && i < m_PWMPorts.size(); i++)
            {
                if (PI::DewChannelLabelsTP[i].getText() != nullptr)
                {
                    int physicalPort = m_PWMPorts[i].portIndex;
                    if (setPortName(physicalPort, PI::DewChannelLabelsTP[i].getText()))
                    {
                        changedCount++;
                    }
                }
            }
            
            if (changedCount > 0)
            {
                LOGF_INFO("Updated %d PWM port label(s)", changedCount);
            }
            
            PI::DewChannelLabelsTP.setState(IPS_OK);
            PI::DewChannelLabelsTP.apply();
            return true;
        }
        
        // Delegate other PowerInterface text properties
        if (PI::processText(dev, name, texts, names, n))
            return true;
    }
    
    return INDI::DefaultDevice::ISNewText(dev, name, texts, names, n);
}

bool BigPowerBoxSimple::saveConfigItems(FILE *fp)
{
    INDI::DefaultDevice::saveConfigItems(fp);
    PI::saveConfigItems(fp);
    return true;
}

bool BigPowerBoxSimple::Handshake()
{
    LOG_DEBUG("Starting handshake...");
    
    PortFD = serialConnection->getPortFD();
    
    if (PortFD <= 0)
    {
        LOGF_ERROR("Invalid port file descriptor during handshake (PortFD=%d).", PortFD);
        return false;
    }
    
    LOGF_DEBUG("Handshake: PortFD=%d", PortFD);
    
    // Wait for connection to stabilize
    LOG_DEBUG("Waiting for connection to stabilize...");
    usleep(500000); // 500ms delay
    
    // Flush any stale data
    LOG_DEBUG("Flushing serial buffers...");
    tcflush(PortFD, TCIOFLUSH);
    
    // Ping device (retry up to 3 times)
    LOG_DEBUG("Starting ping attempts...");
    for (int attempt = 0; attempt < 3; attempt++)
    {
        if (ping())
        {
            LOG_DEBUG("Ping successful!");
            break;
        }
        if (attempt < 2)
        {
            LOGF_WARN("Ping attempt %d failed, retrying...", attempt + 1);
            usleep(500000); // 500ms delay
        }
        else
        {
            LOG_ERROR("Failed to ping device after 3 attempts");
            return false;
        }
    }

    // Discover device
    LOG_DEBUG("Sending discover command...");
    if (!discover())
    {
        LOG_ERROR("Discovery failed");
        return false;
    }

    LOGF_INFO("Device: %s, Version: %s, Signature: %s", 
              m_DeviceName.c_str(), m_Version.c_str(), m_BoardSignature.c_str());

    // Parse board signature
    if (!parseBoardSignature(m_BoardSignature))
    {
        LOG_ERROR("Failed to parse board signature");
        return false;
    }

    // Set PowerInterface capabilities
    uint32_t capabilities = 0;
    if (m_SwitchablePortCount > 0)
        capabilities |= POWER_HAS_DC_OUT;
    if (m_HasPWM)
    {
        capabilities |= POWER_HAS_DEW_OUT;
    }
    // BigPowerBox always provides input voltage and current measurements
    capabilities |= POWER_HAS_VOLTAGE_SENSOR;
    capabilities |= POWER_HAS_OVERALL_CURRENT;
    capabilities |= POWER_HAS_PER_PORT_CURRENT;
    
    PI::SetCapability(capabilities);
    PI::initProperties(
        POWER_TAB,
        m_SwitchablePortCount,  // DC Ports (all switchable ports)
        m_PWMPorts.size(),      // Dew Ports (PWM ports)
        0,                      // Variable Ports (not used - PWM handles this)
        0,                      // Auto Dew Ports
        0                       // USB Ports
    );

    // Query initial status
    getStatus();
    
    // Query port names
    queryPortNames();

    return true;
}

bool BigPowerBoxSimple::sendCommand(const char *command, std::string &response, int timeoutMs)
{
    if (PortFD < 0)
    {
        LOGF_ERROR("Invalid PortFD (%d) in sendCommand", PortFD);
        return false;
    }

    // Flush both input and output buffers to ensure clean state
    tcflush(PortFD, TCIOFLUSH);

    // Send command with newline
    char commandWithNewline[256];
    snprintf(commandWithNewline, sizeof(commandWithNewline), "%s\n", command);
    
    int nbytes_written = 0;
    if (tty_write(PortFD, commandWithNewline, strlen(commandWithNewline), &nbytes_written) != TTY_OK)
    {
        char errmsg[256];
        tty_error_msg(nbytes_written, errmsg, 256);
        LOGF_ERROR("Serial write error: %s", errmsg);
        return false;
    }
    
    LOGF_DEBUG("SEND (%d bytes): %s", nbytes_written, command);
    
    // Wait for data to be transmitted
    tcdrain(PortFD);
    
    // Small delay to allow device to process command
    usleep(100000); // 100ms delay

    // Read response until '#' terminator
    char buffer[512];
    int nbytes_read = 0;
    int timeoutSeconds = (timeoutMs + 999) / 1000; // Convert ms to seconds, rounding up
    LOGF_DEBUG("Reading response (timeout: %d seconds)...", timeoutSeconds);
    
    int tty_result = tty_read_section(PortFD, buffer, '#', timeoutSeconds, &nbytes_read);
    if (tty_result != TTY_OK)
    {
        if (tty_result == TTY_TIME_OUT)
        {
            LOGF_ERROR("Read timeout after %d seconds", timeoutSeconds);
        }
        else
        {
            char errmsg[256];
            tty_error_msg(tty_result, errmsg, 256);
            LOGF_ERROR("Serial read error: %s (code=%d)", errmsg, tty_result);
        }
        return false;
    }

    buffer[nbytes_read] = '\0';
    response = buffer;
    
    LOGF_DEBUG("RECV (%d bytes): %s", nbytes_read, response.c_str());
    return true;
}

bool BigPowerBoxSimple::ping()
{
    LOG_DEBUG("Attempting ping...");
    std::string response;
    if (!sendCommand(">P#", response, TIMEOUT_MSEC))
    {
        LOG_DEBUG("Ping command failed or timed out");
        return false;
    }
    
    // Response should be ">POK#" but may have trailing characters
    bool success = response.find(">POK") != std::string::npos;
    if (!success)
    {
        LOGF_ERROR("Ping failed: Expected '>POK' in response, got: '%s'", response.c_str());
    }
    return success;
}

bool BigPowerBoxSimple::discover()
{
    std::string response;
    if (!sendCommand(">D#", response, TIMEOUT_MSEC))
    {
        LOG_ERROR("Discover command failed or timed out");
        return false;
    }
    
    std::vector<std::string> fields;
    splitFields(response, fields);
    
    // Expected format: >D:name:version:signature#
    if (fields.size() < 4 || fields[0] != "D")
    {
        LOGF_ERROR("Invalid discover response format. Expected >D:name:version:signature#, got: %s", response.c_str());
        return false;
    }
    
    m_DeviceName = fields[1];
    m_Version = fields[2];
    m_BoardSignature = fields[3];
    
    LOGF_INFO("Discovered device: %s (version %s, signature: %s)", 
              m_DeviceName.c_str(), m_Version.c_str(), m_BoardSignature.c_str());
    
    return true;
}

void BigPowerBoxSimple::splitFields(const std::string &response, std::vector<std::string> &fields)
{
    fields.clear();
    
    // Remove '>' and '#' markers
    std::string data = response;
    if (!data.empty() && data[0] == '>')
        data = data.substr(1);
    if (!data.empty() && data.back() == '#')
        data = data.substr(0, data.length() - 1);
    
    // Split by ':'
    std::istringstream iss(data);
    std::string field;
    
    while (std::getline(iss, field, ':'))
    {
        fields.push_back(field);
    }
}

bool BigPowerBoxSimple::parseBoardSignature(const std::string &signature)
{
    m_PortTypes.clear();
    m_PWMPorts.clear();
    m_DCPortMap.clear();
    m_PortCount = 0;
    m_SwitchablePortCount = 0;
    m_HasPWM = false;
    
    // Parse signature character by character
    for (size_t i = 0; i < signature.length(); i++)
    {
        char c = signature[i];
        
        switch (c)
        {
            case 'm':
                m_PortTypes.push_back(PORT_MULTIPLEXED);
                m_DCPortMap.push_back(m_PortCount);  // Map DC port index to physical port
                m_PortCount++;
                m_SwitchablePortCount++;
                break;
            case 'p':
                m_PortTypes.push_back(PORT_PWM);
                m_PortCount++;
                m_HasPWM = true;
                {
                    PWMPortInfo info;
                    info.portIndex = m_PortCount - 1;
                    info.mode = 0;
                    info.tempOffset = 0;
                    m_PWMPorts.push_back(info);
                }
                break;
            case 'a':
                m_PortTypes.push_back(PORT_ALWAYS_ON);
                m_PortCount++;
                break;
            case 's':
                m_PortTypes.push_back(PORT_SWITCHABLE);
                m_DCPortMap.push_back(m_PortCount);  // Map DC port index to physical port
                m_PortCount++;
                m_SwitchablePortCount++;
                break;
            case 'f':
            case 'g':
            case 't':
            case 'h':
                // Temperature/humidity probes (ignored as requested)
                break;
            default:
                // Unknown character, ignore
                break;
        }
    }
    
    return m_PortCount > 0;
}

bool BigPowerBoxSimple::turnPortOn(int port)
{
    char command[16];
    snprintf(command, sizeof(command), ">O:%02d#", port);
    std::string response;
    if (!sendCommand(command, response, TIMEOUT_MSEC))
        return false;
    return response.find(">OOK") != std::string::npos;
}

bool BigPowerBoxSimple::turnPortOff(int port)
{
    char command[16];
    snprintf(command, sizeof(command), ">F:%02d#", port);
    std::string response;
    if (!sendCommand(command, response, TIMEOUT_MSEC))
        return false;
    return response.find(">FOK") != std::string::npos;
}

bool BigPowerBoxSimple::setPWMLevel(int port, int level)
{
    if (level < 0 || level > 255)
        return false;
    
    char command[20];
    snprintf(command, sizeof(command), ">W:%02d:%d#", port, level);
    std::string response;
    if (!sendCommand(command, response, TIMEOUT_MSEC))
        return false;
    return response.find(">WOK") != std::string::npos;
}

bool BigPowerBoxSimple::getPortName(int port, std::string &name)
{
    char command[16];
    snprintf(command, sizeof(command), ">N:%02d#", port);
    std::string response;
    if (!sendCommand(command, response, TIMEOUT_MSEC))
    {
        LOGF_DEBUG("getPortName(%d): sendCommand failed", port);
        return false;
    }
    
    std::vector<std::string> fields;
    splitFields(response, fields);
    
    // Expected format: >N:dd:portname#
    if (fields.size() >= 3 && fields[0] == "N")
    {
        name = fields[2];
        return true;
    }
    
    return false;
}

bool BigPowerBoxSimple::setPortName(int port, const std::string &name)
{
    char command[64];
    snprintf(command, sizeof(command), ">M:%02d:%s#", port, name.c_str());
    std::string response;
    if (!sendCommand(command, response, TIMEOUT_MSEC))
        return false;
    return response.find(">MOK") != std::string::npos;
}

bool BigPowerBoxSimple::getPWMMode(int port, int &mode)
{
    char command[16];
    snprintf(command, sizeof(command), ">G:%02d#", port);
    std::string response;
    if (!sendCommand(command, response, TIMEOUT_MSEC))
        return false;
    
    std::vector<std::string> fields;
    splitFields(response, fields);
    
    // Expected format: >G:dd:mode#
    if (fields.size() >= 3 && fields[0] == "G")
    {
        mode = atoi(fields[2].c_str());
        return true;
    }
    
    return false;
}

bool BigPowerBoxSimple::setPWMMode(int port, int mode)
{
    char command[20];
    snprintf(command, sizeof(command), ">C:%02d:%d#", port, mode);
    std::string response;
    if (!sendCommand(command, response, TIMEOUT_MSEC))
        return false;
    return response.find(">COK") != std::string::npos;
}

bool BigPowerBoxSimple::getTempOffset(int port, int &offset)
{
    char command[16];
    snprintf(command, sizeof(command), ">H:%02d#", port);
    std::string response;
    if (!sendCommand(command, response, TIMEOUT_MSEC))
        return false;
    
    std::vector<std::string> fields;
    splitFields(response, fields);
    
    // Expected format: >H:dd:offset#
    if (fields.size() >= 3 && fields[0] == "H")
    {
        offset = atoi(fields[2].c_str());
        return true;
    }
    
    return false;
}

bool BigPowerBoxSimple::setTempOffset(int port, int offset)
{
    char command[20];
    snprintf(command, sizeof(command), ">T:%02d:%d#", port, offset);
    std::string response;
    if (!sendCommand(command, response, TIMEOUT_MSEC))
        return false;
    return response.find(">TOK") != std::string::npos;
}

bool BigPowerBoxSimple::getStatus()
{
    std::string response;
    if (!sendCommand(">S#", response, TIMEOUT_MSEC))
        return false;
    
    std::vector<std::string> fields;
    splitFields(response, fields);
    
    if (fields.empty() || fields[0] != "S")
    {
        LOG_ERROR("Invalid status response format");
        return false;
    }
    
    updateStatusProperties(fields);
    return true;
}

void BigPowerBoxSimple::updateStatusProperties(const std::vector<std::string> &statusFields)
{
    if (statusFields.size() < 2 || statusFields[0] != "S")
        return;
    
    int statusFieldIndex = 1;  // Start after 'S' command
    int currentFieldIndex = 1 + m_PortCount;  // Currents come after all statuses
    
    // Update port statuses and currents in a single loop
    int dcPortIndex = 0;
    int pwmPortIndex = 0;
    
    for (int i = 0; i < m_PortCount; i++)
    {
        // Read status value
        double statusValue = 0.0;
        if (statusFieldIndex < (int)statusFields.size())
        {
            statusValue = atof(statusFields[statusFieldIndex].c_str());
            statusFieldIndex++;
        }
        
        // Read current value
        double current = 0.0;
        if (currentFieldIndex < (int)statusFields.size())
        {
            current = atof(statusFields[currentFieldIndex].c_str());
            currentFieldIndex++;
        }
        
        // Update PowerInterface port states and currents
        if (m_PortTypes[i] == PORT_MULTIPLEXED || m_PortTypes[i] == PORT_SWITCHABLE)
        {
            // DC ports
            if (dcPortIndex < (int)PI::PowerChannelsSP.size())
            {
                PI::PowerChannelsSP[dcPortIndex].setState(statusValue > 0 ? ISS_ON : ISS_OFF);
            }
            if (dcPortIndex < (int)PI::PowerChannelCurrentNP.size())
            {
                PI::PowerChannelCurrentNP[dcPortIndex].setValue(current);
            }
            dcPortIndex++;
        }
        else if (m_PortTypes[i] == PORT_PWM)
        {
            // PWM ports: status value is the duty cycle (0-255)
            if (pwmPortIndex < (int)PI::DewChannelsSP.size())
            {
                PI::DewChannelsSP[pwmPortIndex].setState(statusValue > 0 ? ISS_ON : ISS_OFF);
            }
            if (pwmPortIndex < (int)PI::DewChannelDutyCycleNP.size())
            {
                // Convert 0-255 to percentage
                PI::DewChannelDutyCycleNP[pwmPortIndex].setValue((statusValue / 255.0) * 100.0);
            }
            if (pwmPortIndex < (int)PI::DewChannelCurrentNP.size())
            {
                PI::DewChannelCurrentNP[pwmPortIndex].setValue(current);
            }
            pwmPortIndex++;
        }
        // Always-on ports are read-only, skip them
    }
    
    // Update fieldIndex to point after currents for input info processing
    int fieldIndex = currentFieldIndex;
    
    // Update input info (current, voltage)
    if (fieldIndex < (int)statusFields.size() && PI::PowerSensorsNP.size() > 1)
    {
        PI::PowerSensorsNP[PI::SENSOR_CURRENT].setValue(atof(statusFields[fieldIndex].c_str())); // Current
        fieldIndex++;
    }
    if (fieldIndex < (int)statusFields.size() && PI::PowerSensorsNP.size() > 0)
    {
        double voltage = atof(statusFields[fieldIndex].c_str());
        PI::PowerSensorsNP[PI::SENSOR_VOLTAGE].setValue(voltage); // Voltage
        if (PI::PowerSensorsNP.size() > 2)
        {
            double current = PI::PowerSensorsNP[PI::SENSOR_CURRENT].getValue();
            PI::PowerSensorsNP[PI::SENSOR_POWER].setValue(voltage * current); // Power
        }
        fieldIndex++;
    }
    
    // Apply property updates
    PI::PowerChannelsSP.apply();
    PI::DewChannelsSP.apply();
    PI::DewChannelDutyCycleNP.apply();
    if (HasPerPortCurrent())
    {
        PI::PowerChannelCurrentNP.apply();
        PI::DewChannelCurrentNP.apply();
    }
    PI::PowerSensorsNP.apply();
}

void BigPowerBoxSimple::queryPortNames()
{
    // Query and update DC port names
    for (size_t dcIndex = 0; dcIndex < m_DCPortMap.size() && dcIndex < PI::PowerChannelLabelsTP.size(); dcIndex++)
    {
        int physicalPort = m_DCPortMap[dcIndex];
        std::string name;
        if (getPortName(physicalPort, name))
        {
            PI::PowerChannelLabelsTP[dcIndex].setText(name.c_str());
        }
        else
        {
            // Use default name
            char defaultName[32];
            snprintf(defaultName, sizeof(defaultName), "Port %d", physicalPort + 1);
            PI::PowerChannelLabelsTP[dcIndex].setText(defaultName);
        }
    }
    if (PI::PowerChannelLabelsTP.size() > 0)
    {
        PI::PowerChannelLabelsTP.apply();
    }
    
    // Query and update PWM port names
    for (size_t pwmIndex = 0; pwmIndex < m_PWMPorts.size() && pwmIndex < PI::DewChannelLabelsTP.size(); pwmIndex++)
    {
        int physicalPort = m_PWMPorts[pwmIndex].portIndex;
        std::string name;
        if (getPortName(physicalPort, name))
        {
            PI::DewChannelLabelsTP[pwmIndex].setText(name.c_str());
        }
        else
        {
            // Use default name
            char defaultName[32];
            snprintf(defaultName, sizeof(defaultName), "Port %d", physicalPort + 1);
            PI::DewChannelLabelsTP[pwmIndex].setText(defaultName);
        }
    }
    if (PI::DewChannelLabelsTP.size() > 0)
    {
        PI::DewChannelLabelsTP.apply();
    }
}

void BigPowerBoxSimple::TimerHit()
{
    if (!isConnected() || !setupComplete)
        return;
    
    // Poll device status
    getStatus();
    
    // Set timer for next update
    SetTimer(UPDATE_INTERVAL_MS);
}

// Implement PowerInterface virtual methods
bool BigPowerBoxSimple::SetPowerPort(size_t port, bool enabled)
{
    // Map PowerInterface DC port index to physical port index
    if (port >= m_DCPortMap.size())
        return false;
    
    int physicalPort = m_DCPortMap[port];
    
    // Verify this is indeed a switchable port
    if (physicalPort >= (int)m_PortTypes.size())
        return false;
    
    if (m_PortTypes[physicalPort] != PORT_MULTIPLEXED && m_PortTypes[physicalPort] != PORT_SWITCHABLE)
        return false;
    
    return enabled ? turnPortOn(physicalPort) : turnPortOff(physicalPort);
}

bool BigPowerBoxSimple::SetDewPort(size_t port, bool enabled, double dutyCycle)
{
    if (port >= m_PWMPorts.size())
        return false;
    
    int level = enabled ? (int)std::round(255.0 * (dutyCycle / 100.0)) : 0;
    if (level < 0) level = 0;
    if (level > 255) level = 255;
    
    return setPWMLevel(m_PWMPorts[port].portIndex, level);
}

bool BigPowerBoxSimple::SetVariablePort(size_t port, bool enabled, double voltage)
{
    INDI_UNUSED(port);
    INDI_UNUSED(enabled);
    INDI_UNUSED(voltage);
    // Not implemented - PWM ports handle variable control via SetDewPort
    return false;
}

bool BigPowerBoxSimple::SetLEDEnabled(bool enabled)
{
    INDI_UNUSED(enabled);
    // Not supported by BigPowerBox
    return false;
}

bool BigPowerBoxSimple::SetAutoDewEnabled(size_t port, bool enabled)
{
    INDI_UNUSED(port);
    INDI_UNUSED(enabled);
    // Auto dew is handled by firmware when PWM mode is set to dew heater mode
    return false;
}

bool BigPowerBoxSimple::CyclePower()
{
    // Not supported by BigPowerBox protocol
    return false;
}

bool BigPowerBoxSimple::SetUSBPort(size_t port, bool enabled)
{
    INDI_UNUSED(port);
    INDI_UNUSED(enabled);
    // Not supported by BigPowerBox
    return false;
}
