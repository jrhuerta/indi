# BigPowerBox Simple Driver

This is a simplified, iterative version of the BigPowerBox driver focused on learning the basics.

## Current Features (v1.0)

This minimal driver implements:

1. **Serial Connection**: Sets up a 9600 baud serial connection
2. **Ping Command**: Implements the `>P#` ping command to verify device communication
3. **Handshake/Discovery**: Implements the `>D#` discover command that retrieves:
   - Device Name
   - Firmware Version  
   - Board Signature (port configuration)

## Building

From the INDI build directory:

```bash
cd /home/jrhuerta/Code/indi/build
cmake ..
make indi_bigpowerbox_simple
```

## Running the Driver

### Start the driver directly:
```bash
./drivers/power/indi_bigpowerbox_simple
```

### Or use with indiserver:
```bash
indiserver -v ./drivers/power/indi_bigpowerbox_simple
```

## Testing

Once running, you can connect to the driver using:
- KStars/Ekos
- INDI Control Panel (`indi_getprop`, `indi_setprop`)
- Any INDI client

### Using indi_getprop to see discovered information:

```bash
# Get the device info
indi_getprop "BigPowerBox Simple.DEVICE_INFO.*"
```

## Protocol Details

### Command Format
Commands are sent with the format: `>CMD:arg1:arg2#\n`
Responses are received with the format: `>RESP:field1:field2#`

### Implemented Commands

1. **Ping**: `>P#`
   - Response: `>POK#`
   - Verifies device is responding

2. **Discover**: `>D#`
   - Response: `>D:name:version:signature#`
   - Example: `>D:BigPowerBox:1.0.0:mmmmmmmmppppaa#`
   
### Board Signature Format

The signature string indicates port types:
- `m` = Multiplexed (MCP23017 switchable port)
- `p` = PWM port (dew heater/variable power)
- `a` = Always-on port
- `s` = Direct switchable port
- `f` = DHT22 temperature/humidity sensor
- `g` = BME280 temperature/humidity/pressure sensor

Example: `mmmmmmmmppppaa` = 8 multiplexed ports, 4 PWM ports, 2 always-on ports

## Next Steps for Iteration

Suggested features to add next:

1. **Parse Board Signature**: Extract port count and types from signature
2. **Status Command**: Implement `>S#` to get current port states and sensor readings
3. **Port Control**: Implement `>O:port#` (turn on) and `>F:port#` (turn off)
4. **Properties**: Add INDI switch properties for each port
5. **PWM Control**: Implement `>W:port:level#` for PWM control
6. **Polling**: Add timer to regularly update status
7. **Full PowerInterface**: Integrate with INDI::PowerInterface

## Debugging

Enable debug mode by setting the DEBUG switch in your INDI client, or via command line:

```bash
indi_setprop "BigPowerBox Simple.DEBUG.ENABLE=On"
```

This will show detailed communication logs including:
- Commands sent
- Responses received
- Handshake process
- Any errors

## File Structure

- `bigpowerbox_simple.h` - Header with class definition
- `bigpowerbox_simple.cpp` - Implementation
- Entry in `CMakeLists.txt` - Build configuration
