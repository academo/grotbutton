# Grot Bot Button

![image](https://github.com/user-attachments/assets/6e0f7b63-a4a8-4458-9a7a-c8dd2a16f3c4)


This is an IoT button in the shape of the Grafana mascot Grot that can trigger webhook requests when pressed.

## Software Requirements

- [PlatformIO](https://platformio.org)

Really the easiest way is to install it as a vscode extension. Even if you don't use vscode as your editor, it is so much easier with the extension.

## Hardware Requirements

- 3D printer parts (get them from [printables](https://www.printables.com/model/1297820-grafana-grot-button-iot-button)
- ESP32-C3 Super Mini (Get it from Aliexpress. Usually less than 2€)
- Cherry MX switch or clone. (You can find clones in Aliexpress for as little as 10 units for 2€.)
- Soldering supplies
- One jumper wire or any other soft wire (0.35 mm² or 22 AWG)
- Super glue or instant glue (Cyanoacrylate-based glue e.g. loctite)

## Firmware instructions

I recommend you to burn the firmware before you assemble the button so you can test your chip is not defective before you solder anything.

1. Clone this repository
2. Open it with PlatformIO
3. Connect the chip to your computer via USB-C 
4. Upload the firmware. In the command palette: `PlatformIO: Upload` or press the Upload button (usually top right corner)
5. Wait for the upload to finish without errors

## Assembly instructions

1. Insert the switch inside the grot bottom part

| ![image](https://github.com/user-attachments/assets/94e51ea0-0715-4f64-a787-8cabbb2c815b) | ![image](https://github.com/user-attachments/assets/acaf4814-3f49-4a78-b822-a846c7f5cdb7) |
|---|---|
   
3. Cut two small jumper wire pieces of approximately 3cm each, and strip the insulation from the ends (about 5mm from each end)

4. Solder one wire to each leg of the switch (DO NOT solder to the chip yet)

5. Glue the housing to the bottom grot part. Pass the wires through the holes in the housing.

   > After this step you will have two wires sticking out of the housing
![image](https://github.com/user-attachments/assets/2c4e9f2f-3737-4b94-8e1a-2d6951d1e62e)


6. Solder the wires to the ESP32 chip. (Keep an eye on the chip orientation. The USB port should point to the housing USB hole)
    * One wire to the G pin (ground)
    * The other wire to the 2 pin (button pin)
    * > NOTE: It doesn't matter which wire connects to which pin

7. Adjust the chip so its usb port is in the housing usb hole


| ![image](https://github.com/user-attachments/assets/317985f8-51fe-4a14-a6e3-b7d150e5029b) | ![image](https://github.com/user-attachments/assets/e656e9c1-7d11-41b8-9404-16c3fff2eafc) |
|---|---|
   


9. Glue the housing lid to the housing (Do not glue the housing to the chip, glue the 3D printed parts)

10. Fix the Grot Top Part to the switch by applying pressure onto the switch until it is pressed and stays in place. 

## How to use?

### First setup 
Once your button is assembled and the firmware is uploaded, you can connect it to any usb c power source (you probably already have it connected to your computer!)

On first boot, the button will start an access point (AP) with the name `GrotBot-<random number>` and no password. Connect to it with your phone or computer.

Once connected (tell your laptop/smartphone to stay connected even if there's no internet), you should get a captive portal to "Sign in". If not, open the web interface at `http://192.168.4.1`

Once you are in the web interface, you can configure the button with your WiFi credentials and webhook URL. (Maybe try with this [webhook-test website](https://webhook-test.com/)?)

Upon saving the configuration, the button will reboot and try to connect to your WiFi network.

If it fails to connect to your WiFi network, it will retry every 10 seconds for a total of 10 attempts. If all attempts fail, it will go back to AP mode and wait for you to configure it again.


### Normal operation

If it successfully connects, you should be able to press the button and see it calls your webhook URL (GET request)

The button will go to sleep after 60 seconds of inactivity to save power. It'll wake up as soon as you press the button.

Pressing the button several times will trigger the webhook for each press.


### Change the webhook URL

To change the webhook URL or wifi credentials:

* Unplug the button
* Keep the button pressed while you plug it back
* Make sure to keep the button pressed for at least 5 seconds while it boots up
* The button will start in AP mode and you can configure it again by connecting to it with your phone or computer
