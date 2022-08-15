# ArduinoDataMonitor
Finite State Machine system that stores channels that include names, minimum and maximum boundaries, current values, and averages of recent values.

This project was completed as coursework. I received a first (96%) for it.

The Arduino stores up to 26 channels of data and displays them on the Adafruit LCD.
![image](https://user-images.githubusercontent.com/109162139/184641294-58e95692-ec08-4c96-bab0-84a20223e2ea.png)

The LCD backlight changes colour to represent channels where their current value falls outside its maximum or minimum boundaries.
![20220327_104030](https://user-images.githubusercontent.com/109162139/184641936-3ad6f646-8c7a-4845-8313-ae53d9722954.jpg)

A large challenge in the project was managing the Arduino's limited memory whilst storing as many recent values as possible for each channel. The Arduino stores the last 20 values given to each channel by compressing the values in batches of 4 and averaging these.
![20220429_152332](https://user-images.githubusercontent.com/109162139/184643154-42f9d54f-fca1-47a1-9684-b675c3e7ac7e.jpg)

