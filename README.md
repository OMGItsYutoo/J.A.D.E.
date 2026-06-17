# J.A.D.E.
J.A.D.E. (Just Another Detection Engine) is an end-to-end project that seamlessly integrates robotics, real-time embedded systems (RTOS), and low-latency networking protocols.
The primary goal is to develop a custom remote controller for the movement and live video monitoring of a SunFounder PiCrawler (https://docs.sunfounder.com/projects/pi-crawler/en/latest/). Thanks to its highly modular design, the system can be easily adapted to interface with any camera-equipped mobile device.
The project stands out for its strictly optimized hardware and software architecture. The controller, powered by an STM32F407 and an ESP32-CAM, leverages an advanced DMA-driven pipeline written in C and FreeRTOS. This design drastically offloads the CPU, ensuring deterministic latencies, asynchronous UDP data reception, and fluid graphical rendering on a TFT display via Double Buffering techniques.

Made for an university project at the Università degli Studi di Napoli Federico II by the students:

- La Salvia Gianmarco [@Ae0nix](https://github.com/Ae0nix)
- Luongo Danilo [@DaniloLuongo](https://github.com/DaniloLuongo)
- Manzo Aldo [@OMGItsYutoo](https://github.com/OMGItsYutoo)
- Marotta Alessio [@alemarotta](https://github.com/alemarotta)
