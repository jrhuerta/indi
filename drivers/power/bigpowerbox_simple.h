#pragma once

#include "defaultdevice.h"
#include <string>
#include <vector>

namespace Connection
{
class Serial;
}

class BigPowerBoxSimple : public INDI::DefaultDevice
{
    public:
        BigPowerBoxSimple();
        virtual ~BigPowerBoxSimple() = default;

        virtual const char *getDefaultName() override;
        virtual bool initProperties() override;
        virtual bool updateProperties() override;
        virtual bool ISNewSwitch(const char *dev, const char *name, ISState *states, char *names[], int n) override;

    private:
    //     // Handshake and communication
        bool Handshake();
    //     bool sendCommand(const char *command, std::string &response, int timeoutMs = 1000);
    //     int PortFD{-1};

    //     // Protocol commands
    //     bool ping();
    //     bool discover();
        
    //     // Helper function
    //     void splitFields(const std::string &response, std::vector<std::string> &fields);
        
     //     // Device information from discover command
     //     std::string m_DeviceName;
     //     std::string m_Version;
     //     std::string m_BoardSignature;
        
        Connection::Serial *serialConnection{nullptr};
        bool m_Initialized{false};
};
