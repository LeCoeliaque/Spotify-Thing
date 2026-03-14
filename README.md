# Spotify-Thing
A small device to view, pause, skip, and shuffle spotify tracks

As someone who listens to music nearly constantly, I hope that this device will be a cool novelty item to sit on my desk or nightstand for easy control without messing with my phone.



# Hardware Setup

This project is designed around 2 key components, the [SeeedStudio Xiao ESP32S3](https://www.seeedstudio.com/XIAO-ESP32S3-p-5627.html) and the [Waveshare 2inch LCD Display Module](https://www.waveshare.com/2inch-lcd-module.htm)



The Xiao has only 1 GND pin so you may need to solder some wires together. 

Wiring Diagram Here

# Code Setup

You will need to setup spotify for developers on the spotify account you intend to use.

1: Go to developer.spotify.com and sign in

2: Create an App, the name and description are irrelevant and can be set to whatever you like, the redirect URI must be set to: https://spotifyesp32.vercel.app/api/spotify/callback

3: Copy the Client ID and Client Secret into the Secrets.h file

4: Fill in your WiFi credentials into Secrets.h

5: Enjoy! 


# Photos
Here are a few photos from the development process

