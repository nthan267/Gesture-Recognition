# Gesture Recognition on Embedded Hardware

This is a gesture recognition system that runs on a small wireless device. No internet. No cloud. You move your hand, it understands what you did.

---

## What it does

It uses a motion sensor instead of a camera. The sensor measures how fast the device is accelerating and rotating in three directions. When you shake it, swipe, tap, or draw a circle in the air, that movement becomes a stream of numbers. A machine learning model — stored directly on the chip — reads those numbers and figures out what you did.

The whole thing happens on hardware smaller than a deck of cards, in under a second, with zero network dependency.

---

## Why not just use the cloud

Most AI today works by sending your data to a server somewhere, waiting for it to think, and getting an answer back. That works fine for a lot of things. But there are situations where it falls apart:

A real-time VR training simulator can't have a 300ms delay every time you move your hand. A device in a factory or a moving vehicle can't depend on a WiFi connection that might drop. A wearable can't be draining its battery uploading data constantly.

The alternative is putting the model on the device itself. The data never leaves. The response is immediate. That's what this project does.

Compared to something like a cloud vision API — which would require a network call, cost money per request, and still introduce latency — this runs for free, offline, on a chip that costs a few dollars.

---

## Where this kind of thing is being used

Gesture recognition on embedded hardware is showing up in a few areas right now:

VR and AR headsets are moving away from physical controllers. Hand tracking and gesture input are the obvious replacement, and they need to work without a server in the loop.

In manufacturing and industrial settings, workers need to interact with systems without touching shared surfaces or removing gloves. Gesture commands are a cleaner solution.

Automotive — controlling in-cabin systems with hand gestures so drivers aren't taking their eyes off the road to tap a screen.

Medical and surgical environments where sterile conditions mean you can't touch a keyboard or controller, but you still need to control what's on a screen.

Accessibility — giving people with limited mobility new ways to interact with devices.

The common thread is that all of these need low latency and can't depend on a stable cloud connection. That's the problem this solves.

---

## What was built

The project covers the full pipeline. Data collection first — a custom system on the board that listens for motion, filters out noise, rejects weak captures, and streams clean data to a CSV file on the PC. Then training on Edge Impulse, which converts the raw sensor readings into a compact neural network that fits on the microcontroller. Then deployment — the model runs directly on the device, classifies each gesture, and shows the result on the built-in screen.

Five gestures: circle, shake, swipe left/right, swipe up/down, tap. Each one has its own detection sensitivity and capture window tuned to how that motion actually feels.

---

## Hardware

**Arduino Nesso N1** — built around the ESP32-C6. 160 MHz, 512 KB RAM, 1.14" color display, buzzer, rechargeable battery. Fits in a palm and runs without being plugged in.

**BMI270 IMU** — 6-axis motion sensor. Reads acceleration and rotation on three axes each, 10 times per second.

