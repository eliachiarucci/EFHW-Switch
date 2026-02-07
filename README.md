# Firmware for EFHW switch

This repository contains the firmware for the [EFHW switch](https://www.morsekey.net/efhw-switch).

The board runs on a Nordic nRF52832 microcontroller, and is powered by a CR2032 coin cell battery.

This project was created using NRF Connect SDK version 2.9.0

A latching relay is used to prolong battery life up to 12 months, depending on usage.

Flashing and debugging is usually done from another Nordic devboard using the official NRF Connect extension and the Segger RTT terminal program.
There are also instructions for an FTDI board and OpenOCD in the FTDI folder.