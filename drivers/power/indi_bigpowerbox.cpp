/*******************************************************************************
  Copyright(c) 2024 BigPowerBox Contributors. All rights reserved.

  BigPowerBox INDI Driver

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

#include "indi_bigpowerbox.h"

#include "indicom.h"
#include "connectionplugins/connectionserial.h"
#include "inditimer.h"

#include <cerrno>
#include <cstring>
#include <cstdio>
#include <algorithm>
#include <sstream>
#include <memory>
#include <termios.h>
#include <unistd.h>
#include <sys/ioctl.h>

// Declaration for an auto pointer to BigPowerBox
static std::unique_ptr<BigPowerBox> bigpowerbox(new BigPowerBox());

#define TIMEOUT_SEC 0
#define TIMEOUT_MSEC 1000
#define UPDATE_INTERVAL_MS 2000

BigPowerBox::BigPowerBox() : INDI::PowerInterface(this)
{
    setVersion(1, 0);
}

const char *BigPowerBox::getDefaultName()
{
    return "BigPowerBox";
}

bool BigPowerBox::initProperties()
{
    INDI::DefaultDevice::initProperties();
    addAuxControls();
    addDebugControl();
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

bool BigPowerBox::updateProperties()
{
    INDI::DefaultDevice::updateProperties();

    if (isConnected())
    {
        // Define Weather sensor properties if sensor present
        if (m_HasWeatherSensor)
        {
            defineProperty(WeatherNP);
        }

        // Define additional properties
        if (m_HasPWM && m_PWMPorts.size() > 0)
        {
            defineProperty(PWMModesNP);
            defineProperty(PWMTempOffsetsNP);
            defineProperty(ResetDewLabelsSP);
        }
        defineProperty(PortCurrentsNP);
        defineProperty(InputInfoNP);
        
        if (m_SwitchablePortCount > 0)
        {
            defineProperty(ResetPowerLabelsSP);
        }

        PI::updateProperties();
        setupComplete = true;
        
        // Start status polling timer
        SetTimer(UPDATE_INTERVAL_MS);
    }
    else
    {
        // Delete properties
        deleteProperty(WeatherNP);
        deleteProperty(PWMModesNP);
        deleteProperty(PWMTempOffsetsNP);
        deleteProperty(PortCurrentsNP);
        deleteProperty(InputInfoNP);
        deleteProperty(ResetPowerLabelsSP);
        deleteProperty(ResetDewLabelsSP);

        PI::updateProperties();
        setupComplete = false;
    }

    return true;
}

bool BigPowerBox::saveConfigItems(FILE *fp)
{
    INDI::DefaultDevice::saveConfigItems(fp);
    PI::saveConfigItems(fp);
    return true;
}

bool BigPowerBox::Handshake()
{
    if (isSimulation())
    {
        // Simulation mode - set up default configuration
        m_BoardSignature = "mmmmmmmmppppaa";
        parseBoardSignature(m_BoardSignature);
        
        uint32_t capabilities = 
            POWER_HAS_DC_OUT |
            POWER_HAS_DEW_OUT |
            POWER_HAS_VARIABLE_OUT |
            POWER_HAS_VOLTAGE_SENSOR |
            POWER_HAS_PER_PORT_CURRENT;
        
        PI::SetCapability(capabilities);
        PI::initProperties(
            POWER_TAB,
            m_SwitchablePortCount,  // DC Ports (all switchable ports)
            m_PWMPorts.size(),  // Dew Ports (PWM ports)
            0,  // Variable Ports (not used - PWM handles this)
            0,  // Auto Dew Ports
            0   // USB Ports
        );
        return true;
    }

    PortFD = serialConnection->getPortFD();
    
    LOGF_DEBUG("Handshake: PortFD=%d", PortFD);
    
    if (PortFD <= 0)
    {
        LOGF_ERROR("Invalid port file descriptor during handshake (PortFD=%d).", PortFD);
        return false;
    }
    
    // Wait a bit for connection to stabilize
    LOG_DEBUG("Waiting for connection to stabilize...");
    usleep(200000); // 200ms delay
    
    // Flush any stale data
    LOG_DEBUG("Flushing serial buffers...");
    tcflush(PortFD, TCIOFLUSH);
    LOG_DEBUG("Starting ping attempts...");

    // Ping device (retry up to 3 times)
    for (int attempt = 0; attempt < 3; attempt++)
    {
        if (ping())
        {
            break;
        }
        if (attempt < 2)
        {
            usleep(500000); // 500ms delay
        }
        else
        {
            LOG_ERROR("Failed to ping device after 3 attempts");
            return false;
        }
    }

    // Discover device
    if (!discover())
    {
        return false;
    }

    // Parse board signature
    if (!parseBoardSignature(m_BoardSignature))
    {
        return false;
    }

    // Set PowerInterface capabilities
    uint32_t capabilities = 
        POWER_HAS_DC_OUT |
        POWER_HAS_DEW_OUT |
        POWER_HAS_VARIABLE_OUT |
        POWER_HAS_VOLTAGE_SENSOR |
        POWER_HAS_PER_PORT_CURRENT;
    
    // Weather is handled separately via WeatherInterface, not PowerInterface

    PI::SetCapability(capabilities);
    PI::initProperties(
        POWER_TAB,
        m_SwitchablePortCount,  // DC Ports (all switchable ports)
        m_PWMPorts.size(),  // Dew Ports (PWM ports)
        0,  // Variable Ports (not used - PWM handles this)
        0,  // Auto Dew Ports
        0   // USB Ports
    );

    // Initialize additional properties
    if (m_HasWeatherSensor)
    {
        int weatherCount = m_HasPressureSensor ? 4 : 3;
        WeatherNP.resize(weatherCount);
        WeatherNP[WEATHER_TEMPERATURE].fill("TEMPERATURE", "Temperature (°C)", "%.1f", -100, 200, 0.1, 0);
        WeatherNP[WEATHER_HUMIDITY].fill("HUMIDITY", "Humidity (%)", "%.1f", 0, 100, 0.1, 0);
        WeatherNP[WEATHER_DEWPOINT].fill("DEWPOINT", "Dewpoint (°C)", "%.1f", -100, 200, 0.1, 0);
        if (m_HasPressureSensor)
        {
            WeatherNP[WEATHER_PRESSURE].fill("PRESSURE", "Pressure (hPa)", "%.1f", 0, 2000, 0.1, 0);
        }
        WeatherNP.fill(getDeviceName(), "WEATHER", "Weather", MAIN_CONTROL_TAB, IP_RO, 0, IPS_IDLE);
    }

    // Port currents
    PortCurrentsNP.resize(m_PortCount);
    for (int i = 0; i < m_PortCount; i++)
    {
        char name[32];
        char label[64];
        snprintf(name, sizeof(name), "CURRENT_%02d", i);
        snprintf(label, sizeof(label), "Port %d Current (A)", i + 1);
        PortCurrentsNP[i].fill(name, label, "%.2f", 0, 50, 0.1, 0);
    }
    PortCurrentsNP.fill(getDeviceName(), "PORT_CURRENTS", "Port Currents", MAIN_CONTROL_TAB, IP_RO, 0, IPS_IDLE);

    // Input info
    InputInfoNP.resize(3);
    InputInfoNP[0].fill("VOLTAGE", "Voltage (V)", "%.2f", 0, 20, 0.1, 0);
    InputInfoNP[1].fill("CURRENT", "Current (A)", "%.2f", 0, 50, 0.1, 0);
    InputInfoNP[2].fill("POWER", "Power (W)", "%.2f", 0, 1000, 0.1, 0);
    InputInfoNP.fill(getDeviceName(), "INPUT_INFO", "Input Info", MAIN_CONTROL_TAB, IP_RO, 0, IPS_IDLE);

    // PWM modes - will be initialized after port names are queried
    if (m_HasPWM && m_PWMPorts.size() > 0)
    {
        PWMModesNP.resize(m_PWMPorts.size());
        for (size_t i = 0; i < m_PWMPorts.size(); i++)
        {
            char name[32];
            // Label will be set later after we query port names
            snprintf(name, sizeof(name), "MODE_%02d", m_PWMPorts[i].portIndex);
            PWMModesNP[i].fill(name, "Channel", "%.0f", 0, 3, 1, 0);
        }
        PWMModesNP.fill(getDeviceName(), "PWM_MODES", "Mode", "PWM Config", IP_RW, 0, IPS_IDLE);

        PWMTempOffsetsNP.resize(m_PWMPorts.size());
        for (size_t i = 0; i < m_PWMPorts.size(); i++)
        {
            char name[32];
            // Label will be set later after we query port names
            snprintf(name, sizeof(name), "OFFSET_%02d", m_PWMPorts[i].portIndex);
            PWMTempOffsetsNP[i].fill(name, "Channel", "%.0f", 0, 10, 1, 0);
        }
        PWMTempOffsetsNP.fill(getDeviceName(), "PWM_TEMP_OFFSETS", "Offset (°C)", "PWM Config", IP_RW, 0, IPS_IDLE);
    }

    // Reset label buttons
    if (m_SwitchablePortCount > 0)
    {
        ResetPowerLabelsSP.resize(1);
        ResetPowerLabelsSP[0].fill("RESET", "Reset to Defaults", ISS_OFF);
        ResetPowerLabelsSP.fill(getDeviceName(), "RESET_POWER_LABELS", "Reset Labels", POWER_TAB, IP_RW, ISR_ATMOST1, 60, IPS_IDLE);
    }
    
    if (m_HasPWM && m_PWMPorts.size() > 0)
    {
        ResetDewLabelsSP.resize(1);
        ResetDewLabelsSP[0].fill("RESET", "Reset to Defaults", ISS_OFF);
        ResetDewLabelsSP.fill(getDeviceName(), "RESET_DEW_LABELS", "Reset Labels", DEW_TAB, IP_RW, ISR_ATMOST1, 60, IPS_IDLE);
    }

    // Query initial status
    getStatus();
    
    // Query port names
    queryPortNames();
    
    // Query PWM modes
    if (m_HasPWM)
    {
        queryPWMModes();
    }

    return true;
}

bool BigPowerBox::sendCommand(const char *command, std::string &response, int timeoutMs)
{
    if (PortFD < 0)
    {
        LOGF_ERROR("Invalid PortFD (%d) in sendCommand", PortFD);
        return false;
    }

    // Flush both input and output buffers to ensure clean state
    tcflush(PortFD, TCIOFLUSH);

    // Send command with newline (device expects line ending after commands)
    char commandWithNewline[256];
    snprintf(commandWithNewline, sizeof(commandWithNewline), "%s\n", command);
    LOGF_DEBUG("Sending command: %s\\n", command);
    LOGF_DEBUG("PortFD=%d, command length=%zu", PortFD, strlen(commandWithNewline));
    
    int nbytes_written = 0;
    if ((nbytes_written = tty_write(PortFD, commandWithNewline, strlen(commandWithNewline), &nbytes_written)) != TTY_OK)
    {
        char errmsg[256];
        tty_error_msg(nbytes_written, errmsg, 256);
        LOGF_ERROR("Serial write error: %s", errmsg);
        return false;
    }
    
    LOGF_DEBUG("Wrote %d bytes, expected %zu bytes", nbytes_written, strlen(commandWithNewline));
    
    // Wait for data to be transmitted
    tcdrain(PortFD);
    
    // Small delay to allow device to process command
    usleep(100000); // 100ms delay

    // Read response until '#' terminator
    // Note: tty_read_section expects timeout in seconds, not milliseconds
    char buffer[512];
    int nbytes_read = 0;
    int timeoutSeconds = (timeoutMs + 999) / 1000; // Convert ms to seconds, rounding up
    LOGF_DEBUG("Reading response (timeout: %d seconds, PortFD=%d)...", timeoutSeconds, PortFD);
    
    int tty_result = tty_read_section(PortFD, buffer, '#', timeoutSeconds, &nbytes_read);
    if (tty_result != TTY_OK)
    {
        if (tty_result == TTY_TIME_OUT)
        {
            LOGF_ERROR("Read timeout after %d seconds (PortFD=%d)", timeoutSeconds, PortFD);
        }
        else
        {
            char errmsg[256];
            tty_error_msg(tty_result, errmsg, 256);
            LOGF_ERROR("Serial read error (PortFD=%d): %s (code=%d)", PortFD, errmsg, tty_result);
        }
        return false;
    }

    buffer[nbytes_read] = '\0';
    response = buffer;
    
    LOGF_DEBUG("Received response (%d bytes): %s", nbytes_read, response.c_str());
    return true;
}

bool BigPowerBox::sendCommandNoResponse(const char *command)
{
    std::string dummy;
    return sendCommand(command, dummy, TIMEOUT_MSEC);
}

bool BigPowerBox::ping()
{
    LOG_DEBUG("Attempting ping...");
    std::string response;
    if (!sendCommand(">P#", response, TIMEOUT_MSEC))
    {
        LOGF_ERROR("Ping command failed or timed out. Response was: '%s'", response.c_str());
        return false;
    }
    
    LOGF_DEBUG("Ping response received: %s", response.c_str());
    
    // Response should be ">POK#" but may have trailing characters
    bool success = response.find(">POK") != std::string::npos || response == ">POK#";
    if (!success)
    {
        LOGF_ERROR("Ping failed: Expected '>POK' in response, got: '%s'", response.c_str());
    }
    return success;
}

bool BigPowerBox::discover()
{
    std::string response;
    if (!sendCommand(">D#", response, TIMEOUT_MSEC))
    {
        LOG_ERROR("Discover command failed or timed out");
        return false;
    }
    
    LOGF_DEBUG("Discover response: %s", response.c_str());
    
    std::vector<std::string> fields;
    splitFields(response, fields);
    
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

bool BigPowerBox::getStatus()
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

bool BigPowerBox::turnPortOn(int port)
{
    char command[16];
    snprintf(command, sizeof(command), ">O:%02d#", port);
    std::string response;
    if (!sendCommand(command, response, TIMEOUT_MSEC))
        return false;
    return response == ">OOK#";
}

bool BigPowerBox::turnPortOff(int port)
{
    char command[16];
    snprintf(command, sizeof(command), ">F:%02d#", port);
    std::string response;
    if (!sendCommand(command, response, TIMEOUT_MSEC))
        return false;
    return response == ">FOK#";
}

bool BigPowerBox::setPWMLevel(int port, int level)
{
    if (level < 0 || level > 255)
        return false;
    
    char command[20];
    snprintf(command, sizeof(command), ">W:%02d:%d#", port, level);
    std::string response;
    if (!sendCommand(command, response, TIMEOUT_MSEC))
        return false;
    return response == ">WOK#";
}

bool BigPowerBox::getPortName(int port, std::string &name)
{
    char command[16];
    snprintf(command, sizeof(command), ">N:%02d#", port);
    std::string response;
    if (!sendCommand(command, response, TIMEOUT_MSEC))
    {
        LOGF_DEBUG("getPortName(%d): sendCommand failed", port);
        return false;
    }
    
    LOGF_DEBUG("getPortName(%d): response = '%s'", port, response.c_str());
    
    std::vector<std::string> fields;
    splitFields(response, fields);
    
    if (fields.size() < 3 || fields[0] != "N")
    {
        LOGF_DEBUG("getPortName(%d): Invalid response format. fields.size()=%zu, fields[0]='%s'", 
                   port, fields.size(), fields.empty() ? "(empty)" : fields[0].c_str());
        return false;
    }
    
    name = fields[2];
    LOGF_DEBUG("getPortName(%d): extracted name = '%s'", port, name.c_str());
    return true;
}

bool BigPowerBox::setPortName(int port, const std::string &name)
{
    if (name.length() > 15)
        return false;
    
    char command[32];
    snprintf(command, sizeof(command), ">M:%02d:%s#", port, name.c_str());
    std::string response;
    if (!sendCommand(command, response, TIMEOUT_MSEC))
        return false;
    return response == ">MOK#";
}

bool BigPowerBox::setPWMMode(int port, int mode)
{
    if (mode < 0 || mode > 3)
        return false;
    
    char command[20];
    snprintf(command, sizeof(command), ">C:%02d:%d#", port, mode);
    std::string response;
    if (!sendCommand(command, response, TIMEOUT_MSEC))
        return false;
    return response == ">COK#";
}

bool BigPowerBox::getPWMMode(int port, int &mode)
{
    char command[16];
    snprintf(command, sizeof(command), ">G:%02d#", port);
    std::string response;
    if (!sendCommand(command, response, TIMEOUT_MSEC))
        return false;
    
    std::vector<std::string> fields;
    splitFields(response, fields);
    
    if (fields.size() < 3 || fields[0] != "G")
    {
        return false;
    }
    
    mode = atoi(fields[2].c_str());
    return true;
}

bool BigPowerBox::setTempOffset(int port, int offset)
{
    if (offset < 0 || offset > 10)
        return false;
    
    char command[20];
    snprintf(command, sizeof(command), ">T:%02d:%d#", port, offset);
    std::string response;
    if (!sendCommand(command, response, TIMEOUT_MSEC))
        return false;
    return response == ">TOK#";
}

bool BigPowerBox::getTempOffset(int port, int &offset)
{
    char command[16];
    snprintf(command, sizeof(command), ">H:%02d#", port);
    std::string response;
    if (!sendCommand(command, response, TIMEOUT_MSEC))
        return false;
    
    std::vector<std::string> fields;
    splitFields(response, fields);
    
    if (fields.size() < 3 || fields[0] != "H")
    {
        return false;
    }
    
    offset = atoi(fields[2].c_str());
    return true;
}

void BigPowerBox::splitFields(const std::string &response, std::vector<std::string> &fields)
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

bool BigPowerBox::parseBoardSignature(const std::string &signature)
{
    m_PortTypes.clear();
    m_PWMPorts.clear();
    m_DCPortMap.clear();
    m_PortCount = 0;
    m_SwitchablePortCount = 0;
    m_HasPWM = false;
    m_HasWeatherSensor = false;
    m_HasPressureSensor = false;
    
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
                m_HasWeatherSensor = true;
                break;
            case 'g':
                m_HasWeatherSensor = true;
                m_HasPressureSensor = true;
                break;
            case 't':
            case 'h':
                // Temperature/humidity probes (handled separately)
                break;
            default:
                // Unknown character, ignore
                break;
        }
    }
    
    return m_PortCount > 0;
}

void BigPowerBox::updateStatusProperties(const std::vector<std::string> &statusFields)
{
    if (statusFields.size() < 2 || statusFields[0] != "S")
        return;
    
    int fieldIndex = 1;
    
    // Update port statuses (one per port in boardSignature order, excluding sensors)
    // Format: port statuses, then PWM duty cycles, then always-on statuses, then currents
    int dcPortIndex = 0;
    int pwmPortIndex = 0;
    
    for (int i = 0; i < m_PortCount && fieldIndex < (int)statusFields.size(); i++)
    {
        double statusValue = atof(statusFields[fieldIndex].c_str());
        
        // Update PowerInterface port states only for switchable ports
        if (m_PortTypes[i] == PORT_MULTIPLEXED || m_PortTypes[i] == PORT_SWITCHABLE)
        {
            if (dcPortIndex < (int)PI::PowerChannelsSP.size())
            {
                PI::PowerChannelsSP[dcPortIndex].setState(statusValue > 0 ? ISS_ON : ISS_OFF);
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
            pwmPortIndex++;
        }
        // Always-on ports are read-only, skip them
        
        fieldIndex++;
    }
    
    // Skip always-on port statuses (hardcoded "1:1:" in firmware)
    // The firmware sends these after PWM ports, but we've already parsed all ports
    // Actually, looking at firmware, always-on comes after PWM, so we need to account for that
    // But since we're parsing in boardSignature order, always-on ports are already included above
    
    // Update port currents (one per port)
    for (int i = 0; i < m_PortCount && fieldIndex < (int)statusFields.size(); i++)
    {
        if (i < (int)PortCurrentsNP.size())
        {
            PortCurrentsNP[i].setValue(atof(statusFields[fieldIndex].c_str()));
        }
        fieldIndex++;
    }
    
    // Update input info
    if (fieldIndex < (int)statusFields.size() && InputInfoNP.size() > 1)
    {
        InputInfoNP[1].setValue(atof(statusFields[fieldIndex].c_str())); // Current
        fieldIndex++;
    }
    if (fieldIndex < (int)statusFields.size() && InputInfoNP.size() > 0)
    {
        InputInfoNP[0].setValue(atof(statusFields[fieldIndex].c_str())); // Voltage
        if (InputInfoNP.size() > 2)
        {
            InputInfoNP[2].setValue(InputInfoNP[0].getValue() * InputInfoNP[1].getValue()); // Power
        }
        fieldIndex++;
    }
    
    // Update weather (if present)
    if (m_HasWeatherSensor && fieldIndex < (int)statusFields.size())
    {
        if (WeatherNP.size() > 0)
        {
            WeatherNP[WEATHER_TEMPERATURE].setValue(atof(statusFields[fieldIndex].c_str()));
            fieldIndex++;
        }
        if (fieldIndex < (int)statusFields.size() && WeatherNP.size() > 1)
        {
            WeatherNP[WEATHER_HUMIDITY].setValue(atof(statusFields[fieldIndex].c_str()));
            fieldIndex++;
        }
        if (fieldIndex < (int)statusFields.size() && WeatherNP.size() > 2)
        {
            WeatherNP[WEATHER_DEWPOINT].setValue(atof(statusFields[fieldIndex].c_str()));
            fieldIndex++;
        }
        if (m_HasPressureSensor && fieldIndex < (int)statusFields.size() && WeatherNP.size() > 3)
        {
            WeatherNP[WEATHER_PRESSURE].setValue(atof(statusFields[fieldIndex].c_str()));
            fieldIndex++;
        }
    }
    
    // Apply property updates
    PI::PowerChannelsSP.apply();
    PI::DewChannelsSP.apply();
    PI::DewChannelDutyCycleNP.apply();
    PortCurrentsNP.apply();
    InputInfoNP.apply();
    if (m_HasWeatherSensor)
    {
        WeatherNP.apply();
    }
}

void BigPowerBox::queryPortNames()
{
    // Map switchable ports (m, s) to PowerChannelLabelsTP
    for (size_t dcIndex = 0; dcIndex < m_DCPortMap.size() && dcIndex < PI::PowerChannelLabelsTP.size(); dcIndex++)
    {
        int physicalPort = m_DCPortMap[dcIndex];
        std::string name;
        if (getPortName(physicalPort, name))
        {
            PI::PowerChannelLabelsTP[dcIndex].setText(name.c_str());
            LOGF_DEBUG("Mapped DC port %zu (physical %d) to label: '%s'", dcIndex, physicalPort, name.c_str());
        }
        else
        {
            // Use default name
            char defaultName[32];
            snprintf(defaultName, sizeof(defaultName), "Port %d", physicalPort + 1);
            PI::PowerChannelLabelsTP[dcIndex].setText(defaultName);
            LOGF_DEBUG("DC port %zu (physical %d) using default label: '%s'", dcIndex, physicalPort, defaultName);
        }
    }
    if (PI::PowerChannelLabelsTP.size() > 0)
    {
        PI::PowerChannelLabelsTP.apply();
    }
    
    // Map PWM ports (p) to DewChannelLabelsTP
    for (size_t pwmIndex = 0; pwmIndex < m_PWMPorts.size() && pwmIndex < PI::DewChannelLabelsTP.size(); pwmIndex++)
    {
        int physicalPort = m_PWMPorts[pwmIndex].portIndex;
        std::string name;
        if (getPortName(physicalPort, name))
        {
            PI::DewChannelLabelsTP[pwmIndex].setText(name.c_str());
            LOGF_DEBUG("Mapped PWM port %zu (physical %d) to label: '%s'", pwmIndex, physicalPort, name.c_str());
        }
        else
        {
            // Use default name
            char defaultName[32];
            snprintf(defaultName, sizeof(defaultName), "Port %d", physicalPort + 1);
            PI::DewChannelLabelsTP[pwmIndex].setText(defaultName);
            LOGF_DEBUG("PWM port %zu (physical %d) using default label: '%s'", pwmIndex, physicalPort, defaultName);
        }
    }
    if (PI::DewChannelLabelsTP.size() > 0)
    {
        PI::DewChannelLabelsTP.apply();
    }
    
    // Update switch and sensor labels to reflect the new names
    syncLabelsToSwitches();
    
    // Update PWM mode and temp offset labels with port names
    updatePWMConfigLabels();
}

void BigPowerBox::updatePWMConfigLabels()
{
    if (!m_HasPWM || m_PWMPorts.size() == 0)
        return;
    
    // Update PWM mode labels with channel names
    for (size_t i = 0; i < m_PWMPorts.size() && i < PWMModesNP.size() && i < PI::DewChannelLabelsTP.size(); i++)
    {
        const char *channelName = PI::DewChannelLabelsTP[i].getText();
        PWMModesNP[i].setLabel(channelName);
        LOGF_DEBUG("Updated PWM mode label %zu to: '%s'", i, channelName);
    }
    
    // Update temp offset labels with channel names
    for (size_t i = 0; i < m_PWMPorts.size() && i < PWMTempOffsetsNP.size() && i < PI::DewChannelLabelsTP.size(); i++)
    {
        const char *channelName = PI::DewChannelLabelsTP[i].getText();
        PWMTempOffsetsNP[i].setLabel(channelName);
        LOGF_DEBUG("Updated PWM temp offset label %zu to: '%s'", i, channelName);
    }
    
    // Redefine properties to update labels in client
    if (isConnected() && PWMModesNP.size() > 0)
    {
        deleteProperty(PWMModesNP);
        defineProperty(PWMModesNP);
    }
    
    if (isConnected() && PWMTempOffsetsNP.size() > 0)
    {
        deleteProperty(PWMTempOffsetsNP);
        defineProperty(PWMTempOffsetsNP);
    }
}

void BigPowerBox::queryPWMModes()
{
    if (!m_HasPWM)
        return;
    
    for (size_t i = 0; i < m_PWMPorts.size(); i++)
    {
        int mode = 0;
        int offset = 0;
        
        if (getPWMMode(m_PWMPorts[i].portIndex, mode))
        {
            m_PWMPorts[i].mode = mode;
        }
        
        if (getTempOffset(m_PWMPorts[i].portIndex, offset))
        {
            m_PWMPorts[i].tempOffset = offset;
        }
    }
    
    // Update properties
    for (size_t i = 0; i < PWMModesNP.size() && i < m_PWMPorts.size(); i++)
    {
        PWMModesNP[i].setValue(m_PWMPorts[i].mode);
    }
    PWMModesNP.apply();
    
    for (size_t i = 0; i < PWMTempOffsetsNP.size() && i < m_PWMPorts.size(); i++)
    {
        PWMTempOffsetsNP[i].setValue(m_PWMPorts[i].tempOffset);
    }
    PWMTempOffsetsNP.apply();
}

void BigPowerBox::syncLabelsToSwitches()
{
    // Update Power Channel switch labels from PowerChannelLabelsTP
    for (size_t i = 0; i < PI::PowerChannelsSP.size() && i < PI::PowerChannelLabelsTP.size(); i++)
    {
        const char *label = PI::PowerChannelLabelsTP[i].getText();
        PI::PowerChannelsSP[i].setLabel(label);
        LOGF_DEBUG("Updated PowerChannelsSP[%zu] label to: '%s'", i, label);
    }
    if (PI::PowerChannelsSP.size() > 0 && isConnected())
    {
        // Delete and redefine property to force label update in client
        deleteProperty(PI::PowerChannelsSP);
        defineProperty(PI::PowerChannelsSP);
    }
    
    // Update Power Channel current sensor labels
    for (size_t i = 0; i < PI::PowerChannelCurrentNP.size() && i < PI::PowerChannelLabelsTP.size(); i++)
    {
        const char *label = PI::PowerChannelLabelsTP[i].getText();
        char currentLabel[MAXINDILABEL];
        snprintf(currentLabel, MAXINDILABEL, "%s (A)", label);
        PI::PowerChannelCurrentNP[i].setLabel(currentLabel);
        LOGF_DEBUG("Updated PowerChannelCurrentNP[%zu] label to: '%s'", i, currentLabel);
    }
    if (PI::PowerChannelCurrentNP.size() > 0 && isConnected() && HasPerPortCurrent())
    {
        // Delete and redefine property to force label update in client
        deleteProperty(PI::PowerChannelCurrentNP);
        defineProperty(PI::PowerChannelCurrentNP);
    }
    
    // Update DEW Channel switch labels from DewChannelLabelsTP
    for (size_t i = 0; i < PI::DewChannelsSP.size() && i < PI::DewChannelLabelsTP.size(); i++)
    {
        const char *label = PI::DewChannelLabelsTP[i].getText();
        PI::DewChannelsSP[i].setLabel(label);
        LOGF_DEBUG("Updated DewChannelsSP[%zu] label to: '%s'", i, label);
    }
    if (PI::DewChannelsSP.size() > 0 && isConnected())
    {
        // Delete and redefine property to force label update in client
        deleteProperty(PI::DewChannelsSP);
        defineProperty(PI::DewChannelsSP);
    }
    
    // Update DEW Channel duty cycle labels
    for (size_t i = 0; i < PI::DewChannelDutyCycleNP.size() && i < PI::DewChannelLabelsTP.size(); i++)
    {
        const char *label = PI::DewChannelLabelsTP[i].getText();
        char dutyLabel[MAXINDILABEL];
        snprintf(dutyLabel, MAXINDILABEL, "%s (%%)", label);
        PI::DewChannelDutyCycleNP[i].setLabel(dutyLabel);
        LOGF_DEBUG("Updated DewChannelDutyCycleNP[%zu] label to: '%s'", i, dutyLabel);
    }
    if (PI::DewChannelDutyCycleNP.size() > 0 && isConnected())
    {
        // Delete and redefine property to force label update in client
        deleteProperty(PI::DewChannelDutyCycleNP);
        defineProperty(PI::DewChannelDutyCycleNP);
    }
    
    // Update DEW Channel current sensor labels
    for (size_t i = 0; i < PI::DewChannelCurrentNP.size() && i < PI::DewChannelLabelsTP.size(); i++)
    {
        const char *label = PI::DewChannelLabelsTP[i].getText();
        char currentLabel[MAXINDILABEL];
        snprintf(currentLabel, MAXINDILABEL, "%s (A)", label);
        PI::DewChannelCurrentNP[i].setLabel(currentLabel);
        LOGF_DEBUG("Updated DewChannelCurrentNP[%zu] label to: '%s'", i, currentLabel);
    }
    if (PI::DewChannelCurrentNP.size() > 0 && isConnected() && HasPerPortCurrent())
    {
        // Delete and redefine property to force label update in client
        deleteProperty(PI::DewChannelCurrentNP);
        defineProperty(PI::DewChannelCurrentNP);
    }
}

void BigPowerBox::TimerHit()
{
    if (!isConnected() || !setupComplete)
        return;
    
    getStatus();
    SetTimer(UPDATE_INTERVAL_MS);
}

bool BigPowerBox::ISNewSwitch(const char *dev, const char *name, ISState *states, char *names[], int n)
{
    // Let parent class handle standard controls (DEBUG, etc.) first
    if (INDI::DefaultDevice::ISNewSwitch(dev, name, states, names, n))
        return true;
    
    if (dev != nullptr && strcmp(dev, getDeviceName()) == 0)
    {
        // Handle Reset Power Labels
        if (ResetPowerLabelsSP.isNameMatch(name))
        {
            ResetPowerLabelsSP.reset();
            ResetPowerLabelsSP.setState(IPS_OK);
            ResetPowerLabelsSP.apply();
            
            // Reset all DC port labels to default "Channel N"
            for (size_t dcIndex = 0; dcIndex < m_DCPortMap.size() && dcIndex < PI::PowerChannelLabelsTP.size(); dcIndex++)
            {
                int physicalPort = m_DCPortMap[dcIndex];
                char defaultName[32];
                snprintf(defaultName, sizeof(defaultName), "Channel %d", static_cast<int>(dcIndex + 1));
                
                // Update device
                if (setPortName(physicalPort, defaultName))
                {
                    // Update PowerInterface label
                    PI::PowerChannelLabelsTP[dcIndex].setText(defaultName);
                    LOGF_INFO("Reset DC port %zu (physical %d) to: '%s'", dcIndex, physicalPort, defaultName);
                }
            }
            
            PI::PowerChannelLabelsTP.setState(IPS_OK);
            PI::PowerChannelLabelsTP.apply();
            syncLabelsToSwitches();
            
            LOG_INFO("Power channel labels reset to defaults");
            return true;
        }
        
        // Handle Reset Dew Labels
        if (ResetDewLabelsSP.isNameMatch(name))
        {
            ResetDewLabelsSP.reset();
            ResetDewLabelsSP.setState(IPS_OK);
            ResetDewLabelsSP.apply();
            
            // Reset all PWM port labels to default "Channel N"
            for (size_t pwmIndex = 0; pwmIndex < m_PWMPorts.size() && pwmIndex < PI::DewChannelLabelsTP.size(); pwmIndex++)
            {
                int physicalPort = m_PWMPorts[pwmIndex].portIndex;
                char defaultName[32];
                snprintf(defaultName, sizeof(defaultName), "Channel %d", static_cast<int>(pwmIndex + 1));
                
                // Update device
                if (setPortName(physicalPort, defaultName))
                {
                    // Update PowerInterface label
                    PI::DewChannelLabelsTP[pwmIndex].setText(defaultName);
                    LOGF_INFO("Reset PWM port %zu (physical %d) to: '%s'", pwmIndex, physicalPort, defaultName);
                }
            }
            
            PI::DewChannelLabelsTP.setState(IPS_OK);
            PI::DewChannelLabelsTP.apply();
            syncLabelsToSwitches();
            
            LOG_INFO("Dew channel labels reset to defaults");
            return true;
        }
        
        // Let PowerInterface handle its switches
        if (PI::processSwitch(dev, name, states, names, n))
            return true;
    }
    
    return false;
}

bool BigPowerBox::ISNewNumber(const char *dev, const char *name, double values[], char *names[], int n)
{
    if (dev != nullptr && strcmp(dev, getDeviceName()) == 0)
    {
        // Handle Dew Channel Duty Cycles - override PowerInterface behavior
        // Setting duty cycle always sends to device and updates switch state accordingly
        if (PI::DewChannelDutyCycleNP.isNameMatch(name))
        {
            int changedCount = 0;
            bool allSuccessful = true;
            
            // Check which duty cycles actually changed before updating
            for (int i = 0; i < n; i++)
            {
                for (size_t dewIndex = 0; dewIndex < PI::DewChannelDutyCycleNP.size(); dewIndex++)
                {
                    if (strcmp(names[i], PI::DewChannelDutyCycleNP[dewIndex].getName()) == 0)
                    {
                        double oldDutyCycle = PI::DewChannelDutyCycleNP[dewIndex].getValue();
                        double newDutyCycle = values[i];
                        
                        // Only send command if duty cycle actually changed
                        if (fabs(oldDutyCycle - newDutyCycle) > 1.0)  // Allow small floating point difference
                        {
                            // If duty cycle > 0, turn channel ON with that duty cycle
                            // If duty cycle = 0, turn channel OFF
                            bool shouldBeOn = (newDutyCycle > 0);
                            
                            if (!SetDewPort(dewIndex, shouldBeOn, newDutyCycle))
                            {
                                allSuccessful = false;
                                LOGF_ERROR("Failed to set duty cycle for dew port %zu", dewIndex);
                            }
                            else
                            {
                                LOGF_INFO("Set dew port %zu duty cycle from %.0f%% to %.0f%% (%s)", 
                                         dewIndex, oldDutyCycle, newDutyCycle, shouldBeOn ? "ON" : "OFF");
                                changedCount++;
                                
                                // Update switch state to match
                                if (dewIndex < PI::DewChannelsSP.size())
                                {
                                    PI::DewChannelsSP[dewIndex].setState(shouldBeOn ? ISS_ON : ISS_OFF);
                                    PI::DewChannelsSP.apply();
                                }
                            }
                        }
                        else
                        {
                            LOGF_DEBUG("Dew port %zu duty cycle unchanged (%.0f%%), skipping", dewIndex, oldDutyCycle);
                        }
                        break;
                    }
                }
            }
            
            // Update property values after checking for changes
            PI::DewChannelDutyCycleNP.update(values, names, n);
            
            if (changedCount > 0)
            {
                LOGF_INFO("Updated %d dew port duty cycle(s)", changedCount);
            }
            
            PI::DewChannelDutyCycleNP.setState(allSuccessful ? IPS_OK : IPS_ALERT);
            PI::DewChannelDutyCycleNP.apply();
            return true;
        }
        
        // Handle PWM modes
        if (PWMModesNP.isNameMatch(name))
        {
            PWMModesNP.update(values, names, n);
            
            for (int i = 0; i < n; i++)
            {
                for (size_t j = 0; j < m_PWMPorts.size(); j++)
                {
                    if (strcmp(names[i], PWMModesNP[j].getName()) == 0)
                    {
                        int mode = (int)PWMModesNP[j].getValue();
                        if (mode < 0) mode = 0;
                        if (mode > 3) mode = 3;
                        
                        m_PWMPorts[j].mode = mode;
                        setPWMMode(m_PWMPorts[j].portIndex, mode);
                        break;
                    }
                }
            }
            
            PWMModesNP.setState(IPS_OK);
            PWMModesNP.apply();
            return true;
        }
        
        // Handle PWM temperature offsets
        if (PWMTempOffsetsNP.isNameMatch(name))
        {
            PWMTempOffsetsNP.update(values, names, n);
            
            for (int i = 0; i < n; i++)
            {
                for (size_t j = 0; j < m_PWMPorts.size(); j++)
                {
                    if (strcmp(names[i], PWMTempOffsetsNP[j].getName()) == 0)
                    {
                        int offset = (int)PWMTempOffsetsNP[j].getValue();
                        if (offset < 0) offset = 0;
                        if (offset > 10) offset = 10;
                        
                        m_PWMPorts[j].tempOffset = offset;
                        setTempOffset(m_PWMPorts[j].portIndex, offset);
                        break;
                    }
                }
            }
            
            PWMTempOffsetsNP.setState(IPS_OK);
            PWMTempOffsetsNP.apply();
            return true;
        }
        
        // Let PowerInterface handle other numbers
        if (PI::processNumber(dev, name, values, names, n))
            return true;
    }
    
    return INDI::DefaultDevice::ISNewNumber(dev, name, values, names, n);
}

bool BigPowerBox::ISNewText(const char *dev, const char *name, char *texts[], char *names[], int n)
{
    if (dev != nullptr && strcmp(dev, getDeviceName()) == 0)
    {
        // Handle PowerInterface label properties
        // Power Channel Labels
        if (PI::PowerChannelLabelsTP.isNameMatch(name))
        {
            int changedCount = 0;
            
            // Check which labels actually changed before updating
            for (int i = 0; i < n; i++)
            {
                for (size_t dcIndex = 0; dcIndex < PI::PowerChannelLabelsTP.size(); dcIndex++)
                {
                    if (strcmp(names[i], PI::PowerChannelLabelsTP[dcIndex].getName()) == 0)
                    {
                        // Check if label actually changed
                        const char *oldLabel = PI::PowerChannelLabelsTP[dcIndex].getText();
                        const char *newLabel = texts[i];
                        
                        if (strcmp(oldLabel, newLabel) != 0)
                        {
                            // Map DC port index to physical port
                            if (dcIndex < m_DCPortMap.size())
                            {
                                int physicalPort = m_DCPortMap[dcIndex];
                                std::string portName = newLabel;
                                if (portName.length() > 15)
                                    portName = portName.substr(0, 15);
                                
                                // Send to device
                                if (setPortName(physicalPort, portName))
                                {
                                    LOGF_INFO("Updated DC port %zu (physical %d) name from '%s' to '%s'", 
                                             dcIndex, physicalPort, oldLabel, portName.c_str());
                                    changedCount++;
                                }
                                else
                                {
                                    LOGF_ERROR("Failed to update DC port %zu (physical %d) name", 
                                              dcIndex, physicalPort);
                                }
                            }
                        }
                        else
                        {
                            LOGF_DEBUG("DC port %zu label unchanged ('%s'), skipping", dcIndex, oldLabel);
                        }
                        break;
                    }
                }
            }
            
            // Let PowerInterface update its internal state after we've checked for changes
            PI::processText(dev, name, texts, names, n);
            
            if (changedCount > 0)
            {
                LOGF_INFO("Updated %d DC port label(s)", changedCount);
                // Update all related switch labels
                syncLabelsToSwitches();
            }
            
            return true;
        }
        
        // DEW Channel Labels
        if (PI::DewChannelLabelsTP.isNameMatch(name))
        {
            int changedCount = 0;
            
            // Check which labels actually changed before updating
            for (int i = 0; i < n; i++)
            {
                for (size_t pwmIndex = 0; pwmIndex < PI::DewChannelLabelsTP.size(); pwmIndex++)
                {
                    if (strcmp(names[i], PI::DewChannelLabelsTP[pwmIndex].getName()) == 0)
                    {
                        // Check if label actually changed
                        const char *oldLabel = PI::DewChannelLabelsTP[pwmIndex].getText();
                        const char *newLabel = texts[i];
                        
                        if (strcmp(oldLabel, newLabel) != 0)
                        {
                            // Map PWM index to physical port
                            if (pwmIndex < m_PWMPorts.size())
                            {
                                int physicalPort = m_PWMPorts[pwmIndex].portIndex;
                                std::string portName = newLabel;
                                if (portName.length() > 15)
                                    portName = portName.substr(0, 15);
                                
                                // Send to device
                                if (setPortName(physicalPort, portName))
                                {
                                    LOGF_INFO("Updated PWM port %zu (physical %d) name from '%s' to '%s'", 
                                             pwmIndex, physicalPort, oldLabel, portName.c_str());
                                    changedCount++;
                                }
                                else
                                {
                                    LOGF_ERROR("Failed to update PWM port %zu (physical %d) name", 
                                              pwmIndex, physicalPort);
                                }
                            }
                        }
                        else
                        {
                            LOGF_DEBUG("PWM port %zu label unchanged ('%s'), skipping", pwmIndex, oldLabel);
                        }
                        break;
                    }
                }
            }
            
            // Let PowerInterface update its internal state after we've checked for changes
            PI::processText(dev, name, texts, names, n);
            
            if (changedCount > 0)
            {
                LOGF_DEBUG("Updated %d PWM port label(s)", changedCount);
                // Update all related switch labels
                syncLabelsToSwitches();
                // Update PWM config labels (modes and temp offsets)
                updatePWMConfigLabels();
            }
            
            return true;
        }
    }
    
    return INDI::DefaultDevice::ISNewText(dev, name, texts, names, n);
}

// Implement PowerInterface virtual methods
bool BigPowerBox::SetPowerPort(size_t port, bool enabled)
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

bool BigPowerBox::SetDewPort(size_t port, bool enabled, double dutyCycle)
{
    if (port >= m_PWMPorts.size())
        return false;
    
    int level = enabled ? (int)std::round(255.0 * (dutyCycle / 100.0)) : 0;
    if (level < 0) level = 0;
    if (level > 255) level = 255;
    
    return setPWMLevel(m_PWMPorts[port].portIndex, level);
}

bool BigPowerBox::SetVariablePort(size_t port, bool enabled, double voltage)
{
    INDI_UNUSED(port);
    INDI_UNUSED(enabled);
    INDI_UNUSED(voltage);
    // Not implemented - PWM ports handle variable control via SetDewPort
    return false;
}

bool BigPowerBox::SetLEDEnabled(bool enabled)
{
    INDI_UNUSED(enabled);
    // Not supported by BigPowerBox
    return false;
}

bool BigPowerBox::SetAutoDewEnabled(size_t port, bool enabled)
{
    INDI_UNUSED(port);
    INDI_UNUSED(enabled);
    // Auto dew is handled by firmware when PWM mode is set to dew heater mode
    return false;
}

bool BigPowerBox::CyclePower()
{
    // Not supported by BigPowerBox protocol
    return false;
}

bool BigPowerBox::SetUSBPort(size_t port, bool enabled)
{
    INDI_UNUSED(port);
    INDI_UNUSED(enabled);
    // Not supported by BigPowerBox
    return false;
}
