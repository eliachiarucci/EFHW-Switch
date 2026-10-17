# Debugging and flashing with an FT2232 board

## Hardware configuration

Connect SWCLK to ADBUS 0.
Connect SWDIO to ADBUS 2 (on some boards, there might be a header to select a series resistor on this line).
Connect GND to GND.
Connect VDD to Vref (optional).
The board needs to be self-powered, providing 3.3V to the programming pins won't work.


## Starting the debug server

```bash
openocd -f ./ft2232hl-swd.cfg -f target/nrf52.cfg \
  -c "init" \
  -c "rtt setup 0x20000000 0x10000 \"SEGGER RTT\"" \
  -c "rtt start" \
  -c "rtt server start 9090 0"
```

(`target/nrf52.cfg` is from the openocd homebrew installation folder)


## Opening the log terminal (in a new terminal)

```bash
telnet localhost 9090
```


## To flash the board

```bash
openocd -f ftdi/ft2232hl-swd.cfg -f target/nrf52.cfg \
  -c "program build/merged.hex verify reset exit"
```

(build folder might be different)
