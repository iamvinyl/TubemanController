# TubemanController

A lightweight ESP-based controller for USB-powered wacky tubemen / air dancers.

TubemanController provides network control of a 5V fan using a simple MOSFET interface, with a built-in web UI for configuration and monitoring.

## Features

* Web-based fan ON/OFF control
* UDP control for external applications and automation
* Configurable UDP port
* Wi-Fi setup through the web interface
* Automatic Wi-Fi network scanning
* Anti-stall behavior to keep the tubeman moving
* Adjustable anti-stall interval and pulse duration
* UDP activity and telemetry logging
* OTA firmware updates
* Persistent configuration

## UDP Commands

Send plain-text UDP commands to the configured port:

```text
on
off
toggle
status
pulse
```

## Hardware

Designed around an ESP8266/D1 Mini controlling a 5V USB fan through an N-channel MOSFET.

The fan should **not** be powered directly from an ESP GPIO pin.

## Setup

Flash the firmware, connect to the controller's setup Wi-Fi network, and use the web interface to select your Wi-Fi network and configure the device.

Once connected, the controller can be operated from its web interface or through UDP commands.

## License

Open-source project for DIY and automation use.
