#pragma once

#include "defaultdevice.h"
#include "indipowerinterface.h"
#include <string>
#include <vector>

namespace Connection
{
class Serial;
}

using PI = INDI::PowerInterface;

class BigPowerBoxSimple : public INDI::DefaultDevice, 
                          public INDI::PowerInterface
{
    public:
        BigPowerBoxSimple();
        virtual ~BigPowerBoxSimple() = default;

        virtual const char *getDefaultName() override;
        virtual bool initProperties() override;
        virtual bool updateProperties() override;
        virtual void TimerHit() override;
        virtual bool ISNewSwitch(const char *dev, const char *name, ISState *states, char *names[], int n) override;
        virtual bool ISNewNumber(const char *dev, const char *name, double values[], char *names[], int n) override;
        virtual bool ISNewText(const char *dev, const char *name, char *texts[], char *names[], int n) override;

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
        int PortFD{-1};

        // Protocol commands
        bool ping();
        bool discover();
        bool getStatus();
        
        // Helper function
        void splitFields(const std::string &response, std::vector<std::string> &fields);
        
        // Device information from discover command
        std::string m_DeviceName;
        std::string m_Version;
        std::string m_BoardSignature;
        
        // Port type information
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
        
        // Port counts
        int m_PortCount{0};
        int m_SwitchablePortCount{0};  // Count of switchable ports (s + m)
        bool m_HasPWM{false};
        
        // Protocol command helpers
        bool turnPortOn(int port);
        bool turnPortOff(int port);
        bool setPWMLevel(int port, int level);
        bool getPortName(int port, std::string &name);
        bool setPortName(int port, const std::string &name);
        bool getPWMMode(int port, int &mode);
        bool setPWMMode(int port, int mode);
        bool getTempOffset(int port, int &offset);
        bool setTempOffset(int port, int offset);
        
        // Helper functions
        bool parseBoardSignature(const std::string &signature);
        void updateStatusProperties(const std::vector<std::string> &statusFields);
        void queryPortNames();
        
        Connection::Serial *serialConnection{nullptr};
        bool m_Initialized{false};
        bool setupComplete{false};
};
