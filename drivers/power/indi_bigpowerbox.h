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

#pragma once

#include "defaultdevice.h"
#include "indipowerinterface.h"

#include <vector>
#include <string>
#include <memory>

namespace Connection
{
class Serial;
}

class BigPowerBox : public INDI::DefaultDevice, public INDI::PowerInterface
{
    public:
        BigPowerBox();
        virtual ~BigPowerBox() = default;

        virtual const char *getDefaultName() override;
        virtual bool initProperties() override;
        virtual bool updateProperties() override;
        virtual void TimerHit() override;
        virtual bool ISNewSwitch(const char *dev, const char *name, ISState *states, char *names[], int n) override;
        virtual bool ISNewText(const char *dev, const char *name, char *texts[], char *names[], int n) override;
        virtual bool ISNewNumber(const char *dev, const char *name, double values[], char *names[], int n) override;

    protected:
        virtual bool saveConfigItems(FILE *fp) override;

        // Implement virtual methods from INDI::PowerInterface
        virtual bool SetPowerPort(size_t port, bool enabled) override;
        virtual bool SetDewPort(size_t port, bool enabled, double dutyCycle) override;
        virtual bool SetVariablePort(size_t port, bool enabled, double voltage) override;
        virtual bool SetLEDEnabled(bool enabled) override;
        virtual bool SetAutoDewEnabled(size_t port, bool enabled) override;
        virtual bool CyclePower() override;
        virtual bool SetUSBPort(size_t port, bool enabled) override;

    private:
        // Handshake and communication
        bool Handshake();
        bool sendCommand(const char *command, std::string &response, int timeoutMs = 1000);
        bool sendCommandNoResponse(const char *command);
        int PortFD{-1};

        // Device information
        std::string m_DeviceName;
        std::string m_Version;
        std::string m_BoardSignature;
        int m_PortCount{0};
        int m_SwitchablePortCount{0};  // Count of switchable ports (s + m)
        bool m_HasPWM{false};
        bool m_HasWeatherSensor{false};
        bool m_HasPressureSensor{false};
        
        // Port type information (by index)
        enum PortType
        {
            PORT_MULTIPLEXED,  // 'm' - MCP23017 switchable
            PORT_PWM,          // 'p' - PWM port
            PORT_ALWAYS_ON,    // 'a' - Always on
            PORT_SWITCHABLE    // 's' - Direct switchable
        };
        std::vector<PortType> m_PortTypes;
        
        // Mapping from PowerInterface DC port index to physical port index
        std::vector<int> m_DCPortMap;
        
        // PWM port information
        struct PWMPortInfo
        {
            int portIndex;      // Physical port index
            int mode;           // Current mode (0-3)
            int tempOffset;     // Temperature offset (0-10)
        };
        std::vector<PWMPortInfo> m_PWMPorts;
        
        // Status polling
        bool setupComplete{false};
        
        // Protocol command helpers
        bool ping();
        bool discover();
        bool getStatus();
        bool turnPortOn(int port);
        bool turnPortOff(int port);
        bool setPWMLevel(int port, int level);
        bool getPortName(int port, std::string &name);
        bool setPortName(int port, const std::string &name);
        bool setPWMMode(int port, int mode);
        bool getPWMMode(int port, int &mode);
        bool setTempOffset(int port, int offset);
        bool getTempOffset(int port, int &offset);
        
        // Helper functions
        bool parseBoardSignature(const std::string &signature);
        void splitFields(const std::string &response, std::vector<std::string> &fields);
        void updateStatusProperties(const std::vector<std::string> &statusFields);
        void queryPortNames();
        void queryPWMModes();
        void syncLabelsToSwitches();
        void updatePWMConfigLabels();
        
        // Weather Sensors
        enum
        {
            WEATHER_TEMPERATURE,   /*!< Temperature */
            WEATHER_HUMIDITY,      /*!< Humidity */
            WEATHER_DEWPOINT,      /*!< Dew Point */
            WEATHER_PRESSURE,      /*!< Pressure (if BME280) */
            N_WEATHER_SENSORS      /*!< Number of weather sensors */
        };
        INDI::PropertyNumber WeatherNP{N_WEATHER_SENSORS};
        
        // Additional properties for PWM configuration
        INDI::PropertyNumber PWMModesNP{0};
        INDI::PropertyNumber PWMTempOffsetsNP{0};
        INDI::PropertyNumber PortCurrentsNP{0};
        INDI::PropertyNumber InputInfoNP{3};
        
        // Reset label buttons
        INDI::PropertySwitch ResetPowerLabelsSP{1};
        INDI::PropertySwitch ResetDewLabelsSP{1};
        
        Connection::Serial *serialConnection{nullptr};
};
